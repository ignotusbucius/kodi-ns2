#!/usr/bin/env python3
"""Build inputstream.adaptive's data folder for the Switch 2 Kodi port.

usage: make-addon-data.py <generated dir> <ISA source data dir> <output dir>

<generated dir> is build/isa/inputstream.adaptive (addon.xml + resources/settings.xml that
Kodi's build_addon() made from the .in templates, with the API <requires> block).

Switch 2 differences from a Linux build of the add-on:
  * library="inputstream.adaptive.so" (plain attribute). Kodi 21 checks the plain "library"
    attribute before any library_<os>; on TARGET_SWITCH2 GetPlatformLibraryName() matches no
    #if branch and returns nullptr, so a library_linux/library_switch2 attribute would be ignored.
    ".so" also matches CCompileInfo::GetSharedLibrarySuffix(), so the add-on is flagged binary.
  * <platform>all</platform>. PlatformSupportsAddon() accepts only "all" on TARGET_SWITCH2.
  * a placeholder file named inputstream.adaptive.so: CAddonDll::GetDllPath() returns "" unless
    the library file exists; DlStubs.cpp's dlopen() then maps the name to the code linked into kodi.bin.
"""
import pathlib
import re
import shutil
import sys

LIBRARY = "inputstream.adaptive.so"
PLACEHOLDER = (
    "inputstream.adaptive is linked statically into kodi.bin on Switch 2.\n"
    "This file only has to exist: Kodi checks for it before it calls dlopen(), which\n"
    "xbmc/platform/switch2/DlStubs.cpp resolves through StaticAddons.cpp.\n"
)


def main(gen: pathlib.Path, src: pathlib.Path, out: pathlib.Path) -> int:
    if out.exists():
        shutil.rmtree(out)
    shutil.copytree(src, out, ignore=shutil.ignore_patterns("*.in"))

    xml = (gen / "addon.xml").read_text(encoding="utf-8")
    xml, n_lib = re.subn(r'library_[a-z0-9_]+="[^"]*"', f'library="{LIBRARY}"', xml)
    xml, n_plat = re.subn(r"<platform>[^<]*</platform>", "<platform>all</platform>", xml)
    if n_lib != 1 or n_plat != 1 or "@" in xml:
        print(f"addon.xml: library={n_lib} platform={n_plat} unexpanded@={'@' in xml}", file=sys.stderr)
        return 1
    (out / "addon.xml").write_text(xml, encoding="utf-8")

    settings = (gen / "resources" / "settings.xml").read_text(encoding="utf-8")
    if "@" in settings:
        print("settings.xml has unexpanded @VARS@", file=sys.stderr)
        return 1
    (out / "resources" / "settings.xml").write_text(settings, encoding="utf-8")

    (out / LIBRARY).write_text(PLACEHOLDER, encoding="ascii")
    files = sorted(p for p in out.rglob("*") if p.is_file())
    print(f"{out}: {len(files)} files, {sum(p.stat().st_size for p in files)} bytes")
    return 0


if __name__ == "__main__":
    if len(sys.argv) != 4:
        print(__doc__, file=sys.stderr)
        sys.exit(2)
    sys.exit(main(*(pathlib.Path(a) for a in sys.argv[1:])))
