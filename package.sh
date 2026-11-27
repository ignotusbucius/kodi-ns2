#!/usr/bin/env bash
# Runs inside kodi-switch2-devkit (via ./build.sh nro). Turns build/kodi.bin into out/switch/kodi.nro and
# copies Kodi's data tree (addons, media, system, userdata) to out/switch/kodi/ for the SD card.
set -euo pipefail
cd /k
# BUILD_DIR/OUT_DIR: build/ and out/ by default (release); build.sh passes others for a diag build.
B="${BUILD_DIR:-build}"
O="${OUT_DIR:-out}"
case "$O" in out|out-*) ;; *) echo "refusing OUT_DIR=$O"; exit 1 ;; esac
ELF=$B/kodi.bin
[ -f "$ELF" ] || { echo "no $ELF: run ./build.sh build first"; exit 1; }
OUT=$O/switch
rm -rf "$O" && mkdir -p "$OUT/kodi"
VER=$(sed -n 's/^ *VERSION_MAJOR \(.*\)$/\1/p' xbmc/version.txt).$(sed -n 's/^ *VERSION_MINOR \(.*\)$/\1/p' xbmc/version.txt)
T=/opt/devkitpro/tools/bin
$T/nacptool --create "Kodi" "Team Kodi / Switch 2 port: ignotusbucius" "$VER" "$B/kodi.nacp"
ICON=icon.jpg; [ -f "$ICON" ] || ICON=/opt/devkitpro/libounce/default_icon.jpg
$T/elf2nro "$ELF" "$OUT/kodi.nro" --icon="$ICON" --nacp="$B/kodi.nacp"
for d in addons media system userdata; do
  [ -d "$B/$d" ] && cp -r "$B/$d" "$OUT/kodi/"
done
# inputstream.adaptive: code linked into kodi.bin, data folder from isa/build.sh data (isa/INTEGRATE.md).
ISA=isa/out/inputstream.adaptive
[ -f "$ISA/addon.xml" ] && cp -r "$ISA" "$OUT/kodi/addons/inputstream.adaptive"
# Python: the static CPython's stdlib (deps/build-deps.sh python3) at special://xbmc/system/python, the
# PYTHONHOME that XBPython.cpp sets on TARGET_SWITCH2 (sys.path: .../lib/python3.11, .../site-packages).
PYLIB=prefix/lib/python3.11
[ -f "$PYLIB/os.py" ] || { echo "no $PYLIB: run ./build.sh deps first"; exit 1; }
mkdir -p "$OUT/kodi/system/python/lib"
cp -r "$PYLIB" "$OUT/kodi/system/python/lib/"
# Pillow for script.module.pil: Kodi installs only its addon.xml (extension xbmc.python.module, library="lib").
# The C modules PIL._imaging/_imagingft/_imagingmath/_imagingmorph are built into libpython3.11.a; the
# pure-Python package (with .pyc) comes from deps/build-deps.sh python3 (prefix/share/script.module.pil/lib/PIL).
PILLIB=prefix/share/script.module.pil/lib
PILADDON="$OUT/kodi/addons/script.module.pil"
[ -f "$PILLIB/PIL/Image.py" ] || { echo "no $PILLIB/PIL: run ./build.sh deps first"; exit 1; }
grep -q 'library="lib"' "$PILADDON/addon.xml" || { echo "$PILADDON/addon.xml: no library=\"lib\""; exit 1; }
cp -r "$PILLIB" "$PILADDON/"
# Pre-installed Python add-ons (addons-bundle/, offline copies of the Omega repo) as bundled add-ons in
# special://xbmc/addons. Kodi inserts every add-on it finds on disk for the first time as DISABLED
# (CAddonDatabase::SyncInstalled) unless addon-manifest.xml names it; listing them optional="true" makes
# them enabled on first start while the user can still disable them (only required ones cannot be).
BUNDLE=addons-bundle/addons
if [ -d "$BUNDLE" ]; then
  # Port patches recorded in addons-bundle/MANIFEST.txt ("| patched: <patch> sha256 <hash>"): the patch file must
  # have that hash and be applied to addons/<id> (it reverses cleanly), else a re-fetch dropped it: refuse.
  while IFS= read -r line; do
    id=${line%% |*}
    p=$(sed -n 's/.*| patched: \([^ ]*\) sha256 \([0-9a-f]\{64\}\).*/\1/p' <<<"$line")
    h=$(sed -n 's/.*| patched: \([^ ]*\) sha256 \([0-9a-f]\{64\}\).*/\2/p' <<<"$line")
    [ -n "$p" ] && [ -n "$h" ] || { echo "MANIFEST: unreadable patched: entry for $id"; exit 1; }
    echo "$h  addons-bundle/$p" | sha256sum -c --status || { echo "addons-bundle/$p: sha256 is not $h"; exit 1; }
    patch -R -p1 --dry-run -s -f -d "$BUNDLE" < "addons-bundle/$p" >/dev/null \
      || { echo "$BUNDLE/$id: addons-bundle/$p is not applied (re-fetched?): apply it with patch -p1 -d $BUNDLE"; exit 1; }
    echo "patched add-on: $id ($p applied)"
  done < <(grep -v '^#' addons-bundle/MANIFEST.txt | grep '| patched: ')
  MANIFEST="$OUT/kodi/system/addon-manifest.xml"
  for a in "$BUNDLE"/*/; do
    id=$(basename "$a")
    [ -f "$a/addon.xml" ] || continue
    cp -r "$a" "$OUT/kodi/addons/$id"
    grep -q ">$id<" "$MANIFEST" || sed -i "s|</addons>|  <addon optional=\"true\">$id</addon>\n</addons>|" "$MANIFEST"
  done
  echo "bundled add-ons: $(ls "$BUNDLE" | wc -l)"
fi
sha256sum "$OUT/kodi.nro"
du -sh "$OUT/kodi" "$OUT/kodi.nro"
