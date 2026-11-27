#!/usr/bin/env python3
"""Post-link guard for usbfs2 in kodi.bin (run by build.sh after every build; host python3 -I, no toolchain).

  G1  the archive Kodi linked (prefix/lib/libusbfs2.a) has the pinned sha256 of the read-only hotplug build
      (volume labels, 4 KiB transfer cap, not -DUSBFS2_XFER_64K), equals homebrew/usbfs2/build/libusbfs2.a, holds exactly
      the 13 library members, and every member is byte-identical to homebrew/usbfs2/build/<member>.
  G2  link map: every member was linked (none was replaced by another archive's definitions).
  G3  every allocated input section of every member placed in kodi.bin (map address) holds the member's bytes,
      except the relocated fields (masked: 4 bytes per instruction/32-bit relocation, 8 per 64-bit one); .bss
      sizes equal; merged string sections and .eh_frame are skipped (the linker merges/rewrites them).
  G4  every global symbol a member defines is defined at most once in kodi.bin and, when present, inside a section
      placed from that member (no other definition took it over); no symbol a member references is --wrap'ed
      in the kodi.bin link (a wrap would reroute usbfs2's call to Kodi code with unchanged usbfs2 bytes).
  G5  kodi.bin defines/references no FatFs write function or helper and no disk_write (usbfs2 checks.py check 1
      list), no libounce S1 USB client code (usb[A-Z]*: usbHs*/usbDs*/usbComms*), no Switch2UsbProbe. Kodi must
      link usbfs2_poll (observe removal and reattach); the external usbfs2_unmount entry point must not be linked.
Exit 0 = all pass (one line per check); 1 = the first failure, printed.
usage: usbfs2-guard.py ELF MAP LIB REFDIR NINJA
"""
import hashlib
import re
import struct
import sys

PIN = 'fb8bf929b280c54e28b3bda1ae2399058ce90a4106dfdd41ce2a3bc3c632a6fb'  # attachment-bound RO, default 4 KiB build
MEMBERS = 'bot.o blk.o part.o mountset.o vfs.o diskio.o ffsystem.o ulog.o usbhs2.o devoptab.o usbfs2.o ff.o ffunicode.o'.split()
# usbfs2 checks.py check 1: FatFs write functions and helpers, plus libounce's usbHs/usbDs/usbComms client code.
DENIED = re.compile(r'^(usb[A-Z]\w*|Switch2UsbProbe'
                    r'|f_(write|sync|unlink|mkdir|rename|truncate|getfree|chmod|utime|setlabel|mkfs|fdisk|expand|putc|puts'
                    r'|printf|forward|chdir|chdrive|getcwd|setcp)|disk_write|sync_window|sync_fs|put_fat|create_chain'
                    r'|remove_chain|dir_register|dir_remove|dir_alloc|dir_clear|store_xdir|fill_first_frag|fill_last_frag'
                    r'|change_bitmap|create_partition|create_xdir|st_clust|st_dword|st_qword|st_word)(\.\w+\.\d+)?$')


def fail(n, msg):
    print(f'usbfs2-guard G{n} FAILED: {msg}')
    sys.exit(1)


def sha(b):
    return hashlib.sha256(b).hexdigest()


def ar_members(data):
    if data[:8] != b'!<arch>\n':
        raise ValueError('not an ar archive')
    pos, names, out, longnames = 8, [], {}, b''
    while pos + 60 <= len(data):
        hdr = data[pos:pos + 60]
        name = hdr[:16].decode().rstrip()
        size = int(hdr[48:58].decode().strip())
        body = data[pos + 60:pos + 60 + size]
        pos += 60 + size + (size & 1)
        if name in ('/', '/SYM64/'):
            continue
        if name == '//':
            longnames = body
            continue
        if name.startswith('/') and name[1:].isdigit():
            off = int(name[1:])
            name = longnames[off:longnames.index(b'/\n', off)].decode()
        else:
            name = name.rstrip('/')
        names.append(name)
        out[name] = body
    return names, out


class Elf:
    def __init__(self, data):
        if data[:4] != b'\x7fELF' or data[4] != 2 or data[5] != 1:
            raise ValueError('not ELF64 LE')
        self.d = data
        shoff, = struct.unpack_from('<Q', data, 0x28)
        shentsize, shnum, shstrndx = struct.unpack_from('<HHH', data, 0x3a)
        self.sh = []
        for i in range(shnum):
            name, typ, flags, addr, off, size, link, info, align, entsize = struct.unpack_from(
                '<IIQQQQIIQQ', data, shoff + i * shentsize)
            self.sh.append(dict(name=name, type=typ, flags=flags, addr=addr, off=off, size=size, link=link, info=info,
                                entsize=entsize))
        stroff = self.sh[shstrndx]['off']
        for s in self.sh:
            s['nm'] = self.cstr(stroff + s['name'])

    def cstr(self, off):
        return self.d[off:self.d.index(b'\0', off)].decode('latin-1')

    def bytes_of(self, s):
        return self.d[s['off']:s['off'] + s['size']]

    def symbols(self):
        out = []
        for s in self.sh:
            if s['type'] != 2:    # SHT_SYMTAB
                continue
            stroff = self.sh[s['link']]['off']
            for i in range(s['size'] // 24):
                name, info, other, shndx, value, size = struct.unpack_from('<IBBHQQ', self.d, s['off'] + i * 24)
                out.append(dict(name=self.cstr(stroff + name), bind=info >> 4, type=info & 15, shndx=shndx,
                                value=value, size=size))
        return out

    def relocs(self, target):
        """offset -> width of every relocated field in section index `target`."""
        out = {}
        for s in self.sh:
            if s['type'] == 4 and s['info'] == target:    # SHT_RELA
                for i in range(s['size'] // 24):
                    off, info, _ = struct.unpack_from('<QQq', self.d, s['off'] + i * 24)
                    t = info & 0xffffffff
                    out[off] = 8 if t in (257, 260) else 2 if t in (259, 262) else 4
        return out

    def vread(self, addr, size):
        for s in self.sh:
            if s['type'] != 8 and s['flags'] & 2 and s['addr'] <= addr and addr + size <= s['addr'] + s['size'] \
                    and s['size']:
                o = s['off'] + addr - s['addr']
                return self.d[o:o + size]
        return None


def parse_map(text, lib_re):
    """Placed input sections of the archive members: [(member, section, addr, size)]."""
    i = text.find('Linker script and memory map')
    if i < 0:
        fail(2, 'no "Linker script and memory map" in the map file')
    lines = text[i:].split('\n')
    out, pending = [], None
    one = re.compile(r'^ (\S+)\s+(0x[0-9a-f]+)\s+(0x[0-9a-f]+)\s+(\S.*)$')
    two = re.compile(r'^\s+(0x[0-9a-f]+)\s+(0x[0-9a-f]+)\s+(\S.*)$')
    for line in lines:
        m = one.match(line)
        if m:
            name, addr, size, f = m.groups()
        else:
            m = two.match(line) if pending else None
            if m:
                name, (addr, size, f) = pending, m.groups()
            else:
                pending = line[1:] if re.match(r'^ \S+$', line) else None
                continue
        pending = None
        mm = lib_re.search(f)
        if mm:
            out.append((mm.group(1), name, int(addr, 16), int(size, 16)))
    return out


def main():
    if len(sys.argv) != 6:
        print(__doc__)
        sys.exit(2)
    elf_path, map_path, lib_path, refdir, ninja = sys.argv[1:]
    # ---- G1
    lib = open(lib_path, 'rb').read()
    ref = open(refdir + '/libusbfs2.a', 'rb').read()
    if sha(lib) != PIN:
        fail(1, f'{lib_path} sha256 {sha(lib)} != pinned {PIN}')
    if sha(ref) != PIN:
        fail(1, f'{refdir}/libusbfs2.a sha256 {sha(ref)} != pinned {PIN} (the reviewed build changed)')
    names, mem = ar_members(lib)
    if sorted(names) != sorted(MEMBERS) or len(names) != len(set(names)):
        fail(1, f'members {names} != {MEMBERS}')
    for n in names:
        r = open(f'{refdir}/{n}', 'rb').read()
        if r != mem[n]:
            fail(1, f'member {n} differs from {refdir}/{n}')
    print(f'G1 libusbfs2.a sha256 {PIN[:16]}.. = pinned = {refdir}/libusbfs2.a; {len(names)} members, each '
          f'byte-identical to {refdir}/<member>')
    # ---- G2
    lib_re = re.compile(re.escape('libusbfs2.a') + r'\(([^)]+)\)')
    placed = parse_map(open(map_path, errors='replace').read(), lib_re)
    seen = {p[0] for p in placed}
    missing = sorted(set(MEMBERS) - seen)
    if missing:
        fail(2, f'members never placed in kodi.bin: {missing}')
    print(f'G2 link map: all {len(MEMBERS)} members linked, {len(placed)} input sections placed')
    # ---- G3
    elf = Elf(open(elf_path, 'rb').read())
    compared = masked = nbss = nmerge = neh = 0
    ranges = {}
    for member, sec, addr, size in placed:
        o = Elf(mem[member])
        idx = [i for i, s in enumerate(o.sh) if s['nm'] == sec]
        if len(idx) != 1:
            fail(3, f'{member}: section {sec} not found once in the member')
        s = o.sh[idx[0]]
        if not s['flags'] & 2:
            continue
        if sec == '.eh_frame':    # unwind tables: the linker rewrites and merges them
            neh += 1
            continue
        if size != s['size'] and not (s['flags'] & 0x10):    # SHF_MERGE may shrink
            fail(3, f'{member} {sec}: placed size {size:#x} != member size {s["size"]:#x}')
        ranges.setdefault(member, []).append((addr, addr + size))
        if s['type'] == 8:    # NOBITS
            nbss += 1
            continue
        if s['flags'] & 0x10:
            nmerge += 1
            continue
        got = elf.vread(addr, size)
        if got is None:
            fail(3, f'{member} {sec}: address {addr:#x} not in a kodi.bin section')
        want = bytearray(o.bytes_of(s))
        got = bytearray(got)
        for off, w in o.relocs(idx[0]).items():
            want[off:off + w] = b'\0' * w
            got[off:off + w] = b'\0' * w
            masked += 1
        if want != got:
            first = next(i for i in range(size) if want[i] != got[i])
            fail(3, f'{member} {sec} at {addr:#x}: bytes differ at +{first:#x} (outside relocated fields)')
        compared += 1
    print(f'G3 {compared} placed sections byte-identical to the members outside {masked} relocated fields; '
          f'{nbss} .bss sizes equal; {nmerge} merged string sections, {neh} .eh_frame (rewritten by the linker) skipped')
    # ---- G4
    syms = elf.symbols()
    gdefs = {}
    for s in syms:
        if s['bind'] in (1, 2) and s['shndx'] != 0:
            gdefs.setdefault(s['name'], []).append(s)
    wraps = set(re.findall(r'--wrap=(\S+)', open(ninja, errors='replace').read()))
    nglob = nlinked = 0
    for member in MEMBERS:
        o = Elf(mem[member])
        for s in o.symbols():
            if s['bind'] not in (1, 2) or not s['name']:
                continue
            if s['shndx'] == 0:
                if s['name'] in wraps:
                    fail(4, f'{member} references {s["name"]}, which kodi.bin wraps (--wrap={s["name"]})')
                continue
            nglob += 1
            d = gdefs.get(s['name'], [])
            if len(d) > 1:
                fail(4, f'{s["name"]} ({member}) defined {len(d)} times in kodi.bin')
            if d:
                nlinked += 1
                v = d[0]['value']
                if not any(a <= v < b or (a == v == b) for a, b in ranges.get(member, [])):
                    fail(4, f'{s["name"]} in kodi.bin at {v:#x} is not in a section placed from {member}')
    print(f'G4 {nglob} usbfs2 global definitions: {nlinked} in kodi.bin, each once and inside its own member\'s '
          f'sections ({nglob - nlinked} dropped by --gc-sections); none of {len(wraps)} --wrap symbols is '
          f'referenced by usbfs2')
    # ---- G5
    bad = sorted({s['name'] for s in syms if DENIED.match(s['name'])})
    if bad:
        fail(5, f'denied symbols in kodi.bin: {bad}')
    names = set(gdefs)
    if 'usbfs2_poll' not in names or 'usbfs2_unmount' in names:
        fail(5, 'expected poll, no external unmount')
    print(f'G5 kodi.bin ({len(syms)} symbols): no FatFs write function/helper, no disk_write, no usbHs*/usbDs*/'
          f'usbComms* (usb[A-Z]*) symbol, no Switch2UsbProbe; poll linked, no external unmount')
    print('usbfs2-guard: PASS')


if __name__ == '__main__':
    main()
