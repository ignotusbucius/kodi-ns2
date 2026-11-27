// usbfs2 part: read-only partition parsing (see part.h). Pure: no IPC, no libnx, no allocation.
#include <string.h>
#include "part.h"
#include "ulog.h"

static uint16_t rd16(const uint8_t* p) { return (uint16_t)(p[0] | p[1] << 8); }
static uint32_t rd32(const uint8_t* p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
static uint64_t rd64(const uint8_t* p) { return (uint64_t)rd32(p) | (uint64_t)rd32(p + 4) << 32; }
static int pow2(uint32_t v) { return v && !(v & (v - 1)); }

uint32_t part_crc32(uint32_t crc, const void* p, size_t n) {
    const uint8_t* b = (const uint8_t*)p;
    crc = ~crc;
    for (size_t i = 0; i < n; i++) {
        crc ^= b[i];
        for (int k = 0; k < 8; k++) crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1)));
    }
    return ~crc;
}

void part_guid_str(const uint8_t* g, char* buf) {
    static const char hx[] = "0123456789ABCDEF";
    static const int order[16] = {3, 2, 1, 0, 5, 4, 7, 6, 8, 9, 10, 11, 12, 13, 14, 15};
    int o = 0;
    for (int i = 0; i < 16; i++) {
        if (i == 4 || i == 6 || i == 8 || i == 10) buf[o++] = '-';
        buf[o++] = hx[g[order[i]] >> 4];
        buf[o++] = hx[g[order[i]] & 15];
    }
    buf[o] = 0;
}

static const char* const kFsName[PART_FS_COUNT] = {"unknown", "FAT", "exFAT", "NTFS", "ext2/3/4", "HFS+", "APFS", "btrfs"};
const char* part_fs_name(int fs) { return (fs >= 0 && fs < PART_FS_COUNT) ? kFsName[fs] : "?"; }
const char* part_scheme_name(int s) {
    return s == PART_SCHEME_WHOLE ? "whole-disk" : s == PART_SCHEME_MBR ? "MBR" : s == PART_SCHEME_GPT ? "GPT" : "none";
}

int part_is_fat_vbr(const uint8_t* s, uint32_t block) {
    (void)block;
    if (s[0] == 0xEB && s[1] == 0x76 && s[2] == 0x90 && !memcmp(s + 3, "EXFAT   ", 8)) return 1;
    if (!((s[0] == 0xEB && s[2] == 0x90) || s[0] == 0xE9)) return 0;
    if (!memcmp(s + 3, "NTFS    ", 8)) return 0;
    const uint32_t bps = rd16(s + 11), spc = s[13], rsv = rd16(s + 14), nfat = s[16];
    const uint32_t tot = rd16(s + 19) ? rd16(s + 19) : rd32(s + 32);
    return bps >= 512 && bps <= 4096 && pow2(bps) && pow2(spc) && rsv >= 1 && (nfat == 1 || nfat == 2) && tot != 0;
}

int part_probe_fs(const uint8_t* p, size_t n) {
    if (n < 512) return PART_FS_UNKNOWN;
    if (!memcmp(p + 3, "EXFAT   ", 8)) return PART_FS_EXFAT;
    if (!memcmp(p + 3, "NTFS    ", 8)) return PART_FS_NTFS;
    if (part_is_fat_vbr(p, 512)) return PART_FS_FAT;
    if (n >= 0x1000 && !memcmp(p + 0x20, "NXSB", 4)) return PART_FS_APFS;
    if (n >= 1024 + 0x3a && rd16(p + 1024 + 0x38) == 0xEF53) return PART_FS_EXT;
    if (n >= 1024 + 2 && (!memcmp(p + 1024, "H+", 2) || !memcmp(p + 1024, "HX", 2))) return PART_FS_HFSPLUS;
    return PART_FS_UNKNOWN;
}

typedef struct {
    PartReadFn rd;
    void* ctx;
    uint32_t block;
    uint64_t blocks;
    PartEntry* out;
    int max, n;
} Scan;

static void add(Scan* s, int scheme, int index, uint64_t start, uint64_t count, uint8_t mbr_type, const uint8_t* guid,
                const char* name) {
    if (count == 0 || start == 0 || start >= s->blocks || count > s->blocks - start) {
        ulog(0, "part: %s #%d start=%llu count=%llu outside the disk (%llu blocks); skipped", part_scheme_name(scheme),
             index, (unsigned long long)start, (unsigned long long)count, (unsigned long long)s->blocks);
        return;
    }
    if (s->n >= s->max) {
        ulog(0, "part: more than %d partitions; #%d skipped", s->max, index);
        return;
    }
    PartEntry* e = &s->out[s->n++];
    memset(e, 0, sizeof(*e));
    e->scheme = scheme;
    e->index = index;
    e->start = start;
    e->count = count;
    e->mbr_type = mbr_type;
    if (guid) memcpy(e->guid, guid, 16);
    for (size_t k = 0; name && k + 1 < sizeof(e->name) && name[k]; k++) e->name[k] = name[k];
}

static int is_ext(uint8_t t) { return t == 0x05 || t == 0x0F || t == 0x85; }

static int mbr_valid(const uint8_t* s) {
    if (s[510] != 0x55 || s[511] != 0xAA) return 0;
    for (int i = 0; i < 4; i++) {
        const uint8_t* e = s + 446 + 16 * i;
        if (e[0] != 0x00 && e[0] != 0x80) return 0;
    }
    return 1;
}

static void scan_ebr(Scan* s, uint64_t ext_start, uint64_t ext_count, uint8_t* sec) {
    uint64_t ebr = ext_start;
    int index = 5;
    for (int k = 0; k < PART_MAX_EBR; k++) {
        if (ebr >= s->blocks || ebr < ext_start || ebr - ext_start >= ext_count) return;
        if (s->rd(s->ctx, ebr, 1, sec) != 0) {
            ulog(0, "part: EBR at %llu unreadable", (unsigned long long)ebr);
            return;
        }
        if (sec[510] != 0x55 || sec[511] != 0xAA) {
            ulog(0, "part: EBR at %llu has no 55AA; chain ends", (unsigned long long)ebr);
            return;
        }
        const uint8_t* e0 = sec + 446;
        const uint8_t* e1 = sec + 462;
        if (e0[4] != 0 && !is_ext(e0[4]) && rd32(e0 + 12)) add(s, PART_SCHEME_MBR, index++, ebr + rd32(e0 + 8), rd32(e0 + 12), e0[4], NULL, NULL);
        if (!is_ext(e1[4]) || rd32(e1 + 8) == 0) return;
        const uint64_t next = ext_start + rd32(e1 + 8);
        if (next <= ebr) {
            ulog(0, "part: EBR chain goes backwards (%llu -> %llu); stopped", (unsigned long long)ebr,
                 (unsigned long long)next);
            return;
        }
        ebr = next;
    }
    ulog(0, "part: EBR chain longer than %d; stopped", PART_MAX_EBR);
}

static void scan_mbr(Scan* s, const uint8_t* s0, uint8_t* sec) {
    uint8_t mbr[64];
    memcpy(mbr, s0 + 446, 64);
    for (int i = 0; i < 4; i++) {
        const uint8_t* e = mbr + 16 * i;
        const uint8_t type = e[4];
        const uint32_t start = rd32(e + 8), count = rd32(e + 12);
        if (type == 0 || count == 0 || type == 0xEE) continue;
        if (is_ext(type)) {
            if (start && start < s->blocks) scan_ebr(s, start, count, sec);
            continue;
        }
        add(s, PART_SCHEME_MBR, i + 1, start, count, type, NULL, NULL);
    }
}

// GPT at LBA 1. 1 = a valid header and entry array were found (entries added), 0 = not a valid GPT.
static int scan_gpt(Scan* s, uint8_t* sec) {
    const uint32_t bs = s->block;
    if (s->blocks < 3 || s->rd(s->ctx, 1, 1, sec) != 0) return 0;
    if (memcmp(sec, "EFI PART", 8)) {
        ulog(0, "part: protective MBR but no 'EFI PART' at LBA 1");
        return 0;
    }
    const uint32_t hsize = rd32(sec + 12), hcrc = rd32(sec + 16);
    if (hsize < 92 || hsize > bs) return 0;
    uint8_t h[512];
    memcpy(h, sec, hsize < sizeof(h) ? hsize : sizeof(h));
    memset(h + 16, 0, 4);
    uint32_t crc = part_crc32(0, h, hsize < sizeof(h) ? hsize : sizeof(h));
    if (hsize > sizeof(h)) crc = part_crc32(crc, sec + sizeof(h), hsize - sizeof(h));
    const uint64_t my_lba = rd64(sec + 24), elba = rd64(sec + 72);
    const uint32_t num = rd32(sec + 80), esize = rd32(sec + 84), acrc = rd32(sec + 88);
    if (crc != hcrc || my_lba != 1) {
        ulog(0, "part: GPT header CRC 0x%08x != 0x%08x or my_lba %llu != 1; backup header not read", crc, hcrc,
             (unsigned long long)my_lba);
        return 0;
    }
    if (num == 0 || num > PART_GPT_MAX_ENTRIES || esize < 128 || esize > bs || bs % esize || elba < 2) {
        ulog(0, "part: GPT entries num=%u size=%u lba=%llu unsupported", num, esize, (unsigned long long)elba);
        return 0;
    }
    const uint64_t bytes = (uint64_t)num * esize, nblk = (bytes + bs - 1) / bs;
    if (elba >= s->blocks || nblk > s->blocks - elba) return 0;
    const int n0 = s->n;
    uint32_t acc = 0, idx = 0;
    for (uint64_t b = 0; b < nblk; b++) {
        if (s->rd(s->ctx, elba + b, 1, sec) != 0) {
            s->n = n0;
            return 0;
        }
        const uint32_t use = (uint32_t)((bytes - b * bs) < bs ? (bytes - b * bs) : bs);
        acc = part_crc32(acc, sec, use);
        for (uint32_t off = 0; off + esize <= use; off += esize) {
            const uint8_t* e = sec + off;
            idx++;
            static const uint8_t zero[16];
            if (!memcmp(e, zero, 16)) continue;
            const uint64_t first = rd64(e + 32), last = rd64(e + 40);
            char name[37];
            for (int c = 0; c < 36; c++) {
                const uint16_t u = rd16(e + 56 + 2 * c);
                name[c] = (u >= 0x20 && u < 0x7f) ? (char)u : (u ? '?' : 0);
                if (!u) break;
            }
            name[36] = 0;
            if (last < first) {
                ulog(0, "part: GPT #%u last < first; skipped", idx);
                continue;
            }
            add(s, PART_SCHEME_GPT, (int)idx, first, last - first + 1, 0, e, name);
        }
    }
    if (acc != acrc) {
        ulog(0, "part: GPT entry array CRC 0x%08x != 0x%08x; table ignored", acc, acrc);
        s->n = n0;
        return 0;
    }
    return 1;
}

int part_scan(PartReadFn rd, void* ctx, uint32_t block, uint64_t blocks, PartEntry* out, int max, int* scheme) {
    uint8_t sec[4096];
    *scheme = PART_SCHEME_NONE;
    if (block < 512 || block > sizeof(sec) || !blocks || !out || max <= 0) return -1;
    Scan s = {.rd = rd, .ctx = ctx, .block = block, .blocks = blocks, .out = out, .max = max, .n = 0};
    if (rd(ctx, 0, 1, sec) != 0) return -1;
    if (part_is_fat_vbr(sec, block)) {    // no partition table: the volume starts at LBA 0
        *scheme = PART_SCHEME_WHOLE;
        PartEntry* e = &out[0];
        memset(e, 0, sizeof(*e));
        e->scheme = PART_SCHEME_WHOLE;
        e->index = 0;
        e->start = 0;
        e->count = blocks;
        return 1;
    }
    if (!mbr_valid(sec)) {
        ulog(0, "part: sector 0 is neither a FAT/exFAT boot record nor an MBR (sig %02x%02x)", sec[510], sec[511]);
        return 0;
    }
    int protective = 0;
    for (int i = 0; i < 4; i++)
        if (sec[446 + 16 * i + 4] == 0xEE) protective = 1;
    if (protective) {
        uint8_t s0[512];
        memcpy(s0, sec, sizeof(s0));
        if (scan_gpt(&s, sec)) {
            *scheme = PART_SCHEME_GPT;
            return s.n;
        }
        memcpy(sec, s0, sizeof(s0));
    }
    *scheme = PART_SCHEME_MBR;
    scan_mbr(&s, sec, sec);
    return s.n;
}
