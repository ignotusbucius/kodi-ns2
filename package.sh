#!/usr/bin/env bash
# Runs inside kodi-switch2-devkit (via ./build.sh nro). Turns build/kodi.bin into out/switch/kodi.nro and
# copies Kodi's data tree (addons, media, system, userdata) to out/switch/kodi/ for the SD card.
set -euo pipefail
cd /k
ELF=build/kodi.bin
[ -f "$ELF" ] || { echo "no $ELF: run ./build.sh build first"; exit 1; }
OUT=out/switch
rm -rf out && mkdir -p "$OUT/kodi"
VER=$(sed -n 's/^ *VERSION_MAJOR \(.*\)$/\1/p' xbmc/version.txt).$(sed -n 's/^ *VERSION_MINOR \(.*\)$/\1/p' xbmc/version.txt)
T=/opt/devkitpro/tools/bin
$T/nacptool --create "Kodi" "Team Kodi / Switch 2 port: ignotusbucius" "$VER" build/kodi.nacp
ICON=icon.jpg; [ -f "$ICON" ] || ICON=/opt/devkitpro/libounce/default_icon.jpg
$T/elf2nro "$ELF" "$OUT/kodi.nro" --icon="$ICON" --nacp=build/kodi.nacp
for d in addons media system userdata; do
  [ -d "build/$d" ] && cp -r "build/$d" "$OUT/kodi/"
done
sha256sum "$OUT/kodi.nro"
du -sh "$OUT/kodi" "$OUT/kodi.nro"
