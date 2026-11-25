#!/usr/bin/env bash
# inputstream.adaptive (ISA) for the Switch 2 Kodi port, built as static libraries.
# Runs on the host; every compile step runs in the kodi-switch2-devkit image with ../ mounted at /k.
# Never touches ../build (Kodi's build tree) or the console.
#
#   ./build.sh fetch      clone ISA 21.5.26-Omega + download its pinned deps (sha256-checked), apply patches/
#   ./build.sh deps       bento4 (xbmc fork 1.6.0-641-3-Omega), pugixml 1.13, rapidjson headers -> prefix/
#   ./build.sh isa        ISA's own CMakeLists with cmake/kodi/KodiConfig.cmake, static, no DRM -> build/isa
#   ./build.sh pack       prelink ISA + deps into one object, keep only the ADDON_* entry points global,
#                         rename them isa_ADDON_*            -> out/lib/libkodiaddon_inputstream.adaptive.a
#   ./build.sh data       add-on data folder                -> out/inputstream.adaptive/
#   ./build.sh linktest   StaticAddons.cpp + DlStubs.cpp + the packed add-on -> out/linktest/
#   ./build.sh all
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
KODI="$(cd "$HERE/.." && pwd)"
IMAGE=kodi-switch2-devkit
JOBS="${JOBS:-$(nproc)}"
ISA_TAG=21.5.26-Omega
run() { podman run --rm --security-opt label=disable -v "$KODI:/k" -w /k/isa "$IMAGE" bash -lc ". /opt/devkitpro/switch2vars.sh; set -euo pipefail; $*"; }

fetch() {
  mkdir -p "$HERE/src/dl"
  [ -d "$HERE/src/inputstream.adaptive" ] ||
    git clone -q --depth 1 --branch "$ISA_TAG" https://github.com/xbmc/inputstream.adaptive.git "$HERE/src/inputstream.adaptive"
  local D="$HERE/src/inputstream.adaptive/depends/common" n url
  for n in bento4 pugixml rapidjson; do
    url=$(awk '{print $2}' "$D/$n/$n.txt")
    [ -f "$HERE/src/dl/$n.tar.gz" ] || curl -sfL -o "$HERE/src/dl/$n.tar.gz" "$url"
    echo "$(cat "$D/$n/$n.sha256")  $HERE/src/dl/$n.tar.gz" | sha256sum -c -
    if [ ! -d "$HERE/src/$n" ]; then mkdir -p "$HERE/src/$n"; tar -C "$HERE/src/$n" --strip-components=1 -xzf "$HERE/src/dl/$n.tar.gz"; fi
  done
  local p
  for p in "$HERE"/patches/*.patch; do
    if git -C "$HERE/src/inputstream.adaptive" apply --check "$p" 2>/dev/null; then
      git -C "$HERE/src/inputstream.adaptive" apply "$p"; echo "applied $(basename "$p")"
    else
      git -C "$HERE/src/inputstream.adaptive" apply --check -R "$p" && echo "already applied $(basename "$p")"
    fi
  done
}

deps() {
  run "
    cmake -S src/bento4 -B build/bento4 -G Ninja -DCMAKE_TOOLCHAIN_FILE=/k/isa/toolchain-isa.cmake \
          -DCMAKE_BUILD_TYPE=Release -DBUILD_APPS=OFF -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
          -DCMAKE_INSTALL_PREFIX=/k/isa/prefix -DCMAKE_INSTALL_INCLUDEDIR=include -DCMAKE_INSTALL_LIBDIR=lib >/dev/null
    cmake --build build/bento4 -j$JOBS && cmake --install build/bento4 >/dev/null
    cmake -S src/pugixml -B build/pugixml -G Ninja -DCMAKE_TOOLCHAIN_FILE=/k/isa/toolchain-isa.cmake \
          -DCMAKE_BUILD_TYPE=Release -DCMAKE_POLICY_VERSION_MINIMUM=3.5 -DBUILD_SHARED_LIBS=OFF \
          -DCMAKE_POSITION_INDEPENDENT_CODE=ON -DPUGIXML_BUILD_TESTS=OFF \
          -DCMAKE_INSTALL_PREFIX=/k/isa/prefix -DCMAKE_INSTALL_INCLUDEDIR=include -DCMAKE_INSTALL_LIBDIR=lib >/dev/null
    cmake --build build/pugixml -j$JOBS && cmake --install build/pugixml >/dev/null
    rm -rf prefix/include/rapidjson && cp -r src/rapidjson/include/rapidjson prefix/include/
    ls -l prefix/lib/*.a"
}

isa() {
  run "
    cmake -S src/inputstream.adaptive -B build/isa -G Ninja -DCMAKE_TOOLCHAIN_FILE=/k/isa/toolchain-isa.cmake \
          -DCMAKE_BUILD_TYPE=Release -DCMAKE_EXPORT_COMPILE_COMMANDS=ON -DKodi_DIR=/k/isa/cmake/kodi \
          -DENABLE_INTERNAL_BENTO4=OFF -DBUILD_TESTING=OFF -DISA_NO_DRM=ON \
          -DCMAKE_INSTALL_PREFIX=/k/isa/build/isa-install
    cmake --build build/isa -j$JOBS
    ls -l build/isa/*.a build/isa/lib/*/*.a"
}

# The add-on behaves like a shared object: one prelinked relocatable object in which only the three
# ADDON_* entry points stay global (renamed isa_ADDON_*). Everything else ISA defines - its own
# classes, bento4, pugixml, rapidjson and kodi-dev-kit template/inline copies, and
# kodi::addon::CPrivateBase::m_interface - becomes local, so it cannot collide with Kodi or with a
# second static add-on built from the same dev-kit headers. Undefined references (libc, libstdc++,
# libm, pthread) stay global and resolve against kodi.bin's own link.
pack() {
  run "
    mkdir -p out/lib build/pack
    LD=aarch64-none-elf-ld; OBJCOPY=aarch64-none-elf-objcopy
    \$LD -r --force-group-allocation -o build/pack/isa-prelink.o \
         -u ADDON_Create -u ADDON_GetTypeVersion -u ADDON_GetTypeMinVersion \
         --whole-archive build/isa/inputstream.adaptive.a --no-whole-archive \
         build/isa/lib/mpegts/libmpegts.a build/isa/lib/webm_parser/libwebm_parser.a \
         prefix/lib/libap4.a prefix/lib/libpugixml.a
    # Pass 1 renames the entry points; pass 2 localizes every other defined global.
    \$OBJCOPY --redefine-sym ADDON_Create=isa_ADDON_Create \
         --redefine-sym ADDON_GetTypeVersion=isa_ADDON_GetTypeVersion \
         --redefine-sym ADDON_GetTypeMinVersion=isa_ADDON_GetTypeMinVersion \
         build/pack/isa-prelink.o build/pack/isa-renamed.o
    \$OBJCOPY --keep-global-symbol=isa_ADDON_Create --keep-global-symbol=isa_ADDON_GetTypeVersion \
         --keep-global-symbol=isa_ADDON_GetTypeMinVersion \
         build/pack/isa-renamed.o build/pack/kodiaddon_inputstream.adaptive.o
    rm -f out/lib/libkodiaddon_inputstream.adaptive.a
    aarch64-none-elf-ar rcs out/lib/libkodiaddon_inputstream.adaptive.a build/pack/kodiaddon_inputstream.adaptive.o
    echo '--- global defined symbols:'
    aarch64-none-elf-nm -g --defined-only build/pack/kodiaddon_inputstream.adaptive.o
    ls -l out/lib/"
}

data() {
  python3 "$HERE/make-addon-data.py" "$HERE/build/isa/inputstream.adaptive" \
          "$HERE/src/inputstream.adaptive/inputstream.adaptive" "$HERE/out/inputstream.adaptive"
}

linktest() {
  run "
    mkdir -p out/linktest
    CXX=aarch64-none-elf-g++
    CXXFLAGS='-mcpu=cortex-a78c+crypto -mtp=soft -ftls-model=local-exec -fPIC -O2 -std=c++17 -ffunction-sections -fdata-sections -D__SWITCH__ -D__SWITCH2__ -DTARGET_POSIX -DTARGET_SWITCH2 -I/k/xbmc/xbmc/platform/switch2 -DKODI_STATIC_ADDON_INPUTSTREAM_ADAPTIVE'
    \$CXX \$CXXFLAGS -c /k/xbmc/xbmc/platform/switch2/StaticAddons.cpp -o build/pack/StaticAddons.o
    \$CXX \$CXXFLAGS -c /k/xbmc/xbmc/platform/switch2/DlStubs.cpp -o build/pack/DlStubs.o
    \$CXX \$CXXFLAGS -c linktest/linktest_main.cpp -o build/pack/linktest_main.o
    \$CXX \$CXXFLAGS -c linktest/kodi_side_stubs.cpp -o build/pack/kodi_side_stubs.o
    # 1. Strict: what the add-on needs that neither the toolchain libraries nor our two files give.
    \$CXX -specs=/opt/devkitpro/libounce/switch.specs -fPIE -L/opt/devkitpro/libounce/lib \
          -Wl,--gc-sections -Wl,--unresolved-symbols=report-all -Wl,--noinhibit-exec \
          -Wl,-Map,out/linktest/linktest.map -o out/linktest/linktest.elf \
          build/pack/linktest_main.o build/pack/StaticAddons.o build/pack/DlStubs.o \
          out/lib/libkodiaddon_inputstream.adaptive.a -lnx -lm 2> out/linktest/link-stderr.txt || true
    grep -o \"undefined reference to \\\`[^']*'\" out/linktest/link-stderr.txt | sort -u > out/linktest/undefined.txt || true
    echo \"undefined symbols: \$(wc -l < out/linktest/undefined.txt)\"; cat out/linktest/undefined.txt
    # 2. Clean: with the kodi.bin-side symbols stood in, it must link with zero errors.
    \$CXX -specs=/opt/devkitpro/libounce/switch.specs -fPIE -L/opt/devkitpro/libounce/lib \
          -Wl,--gc-sections -Wl,-Map,out/linktest/linktest-clean.map -o out/linktest/linktest-clean.elf \
          build/pack/linktest_main.o build/pack/StaticAddons.o build/pack/DlStubs.o build/pack/kodi_side_stubs.o \
          out/lib/libkodiaddon_inputstream.adaptive.a -lnx -lm
    # 3. Baseline: the same program without the add-on and without StaticAddons.o (weak registry
    #    references stay null): this is what kodi.bin gets until INTEGRATE.md is applied.
    \$CXX -specs=/opt/devkitpro/libounce/switch.specs -fPIE -L/opt/devkitpro/libounce/lib \
          -Wl,--gc-sections -o out/linktest/linktest-baseline.elf \
          build/pack/linktest_main.o build/pack/DlStubs.o -lnx -lm
    echo linktest-clean: exit \$?
    aarch64-none-elf-size out/linktest/*.elf"
}

case "${1:-all}" in
  fetch) fetch ;; deps) deps ;; isa) isa ;; pack) pack ;; data) data ;; linktest) linktest ;;
  all) fetch; deps; isa; pack; data; linktest ;;
  *) echo "usage: $0 {fetch|deps|isa|pack|data|linktest|all}" >&2; exit 2 ;;
esac
