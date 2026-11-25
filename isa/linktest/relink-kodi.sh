#!/usr/bin/env bash
# Offline check: relink Kodi's current objects (read-only from /k/build) twice, with and without the
# packed add-on + registry, into /k/isa/out/linktest/. Never writes under /k/build.
# Runs inside the devkit image: podman run ... -v homebrew/kodi:/k kodi-switch2-devkit bash /k/isa/linktest/relink-kodi.sh
set -euo pipefail
. /opt/devkitpro/switch2vars.sh
B=/k/build; O=/k/isa/out/linktest; P=/k/isa/build/pack
mkdir -p "$O" "$P/kodi"
cd "$B"
stmt=$(awk '/^build kodi.bin:/{f=1} f{print} f&&/TARGET_PDB/{exit}' build.ninja)
FLAGS=$(sed -n 's/^  FLAGS = //p' <<<"$stmt")
LINK_FLAGS=$(sed -n 's/^  LINK_FLAGS = //p' <<<"$stmt")
LIBS=$(sed -n 's/^  LINK_LIBRARIES = //p' <<<"$stmt")
# Archives that do not exist yet (another worker is mid-build) are dropped and reported.
KEEP=""; for w in $LIBS; do
  if [[ $w == build/*.a && ! -f $w ]]; then echo "relink: skipping missing $w" >&2; continue; fi
  KEEP+=" $w"; done
# platform_switch2.a with the new DlStubs.cpp + StaticAddons.cpp, compiled with Kodi's own flags.
CC_DL=$(python3 -c "import json;print([e['command'] for e in json.load(open('compile_commands.json')) if e['file'].endswith('platform/switch2/DlStubs.cpp')][0])")
for f in DlStubs StaticAddons; do
  cmd=$(sed -e "s# -o [^ ]*DlStubs.cpp.obj# -o $P/kodi/$f.o#" -e "s# -c [^ ]*DlStubs.cpp# -c /k/xbmc/xbmc/platform/switch2/$f.cpp#" <<<"$CC_DL")
  eval "$cmd -DKODI_STATIC_ADDON_INPUTSTREAM_ADAPTIVE"
done
cp build/platform/switch2/platform_switch2.a "$P/kodi/platform_switch2.a"
aarch64-none-elf-ar d "$P/kodi/platform_switch2.a" DlStubs.cpp.obj
aarch64-none-elf-ar rs "$P/kodi/platform_switch2.a" "$P/kodi/DlStubs.o" "$P/kodi/StaticAddons.o"
WITH=${KEEP//build\/platform\/switch2\/platform_switch2.a/$P/kodi/platform_switch2.a}
link() { # $1 output, rest extra
  local out=$1; shift
  aarch64-none-elf-g++ $FLAGS $LINK_FLAGS CMakeFiles/kodi.dir/xbmc/platform/switch2/main.cpp.obj -o "$out" \
    $@ -Wl,--unresolved-symbols=report-all -Wl,--noinhibit-exec 2> "$out.stderr" || true
  grep -o "undefined reference to \`[^']*'" "$out.stderr" | sort -u > "$out.undefined" || true
  grep -c "multiple definition" "$out.stderr" > "$out.multidef" || true
}
link "$O/kodi-baseline.elf" $KEEP
link "$O/kodi-isa.elf" $WITH /k/isa/out/lib/libkodiaddon_inputstream.adaptive.a
echo "baseline undefined: $(wc -l < $O/kodi-baseline.elf.undefined)  multidef: $(cat $O/kodi-baseline.elf.multidef)"
echo "with ISA undefined: $(wc -l < $O/kodi-isa.elf.undefined)  multidef: $(cat $O/kodi-isa.elf.multidef)"
echo "undefined only with ISA:"; comm -13 "$O/kodi-baseline.elf.undefined" "$O/kodi-isa.elf.undefined"
aarch64-none-elf-size "$O/kodi-baseline.elf" "$O/kodi-isa.elf"
aarch64-none-elf-nm "$O/kodi-isa.elf" | grep -w "isa_ADDON_Create\|isa_ADDON_GetTypeVersion\|isa_ADDON_GetTypeMinVersion\|kodi_static_addon_find"
