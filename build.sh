#!/usr/bin/env bash
# Kodi for Switch 2 — build driver. Run on the host; every step runs in the kodi-switch2-devkit image.
#   ./build.sh hosttools   native TexturePacker + JsonSchemaBuilder + flatc  -> hosttools/bin
#   ./build.sh deps        static libs the devkit lacks, CPython 3.11, groovy -> prefix/
#   ./build.sh configure   cmake configure of xbmc/ for switch2 (SWITCH2_DIAG=ON: diagnostics build) -> build/
#   ./build.sh build       compile + link kodi.elf
#   ./build.sh nro         elf2nro + data tree                              -> out/
#   ./build.sh all
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
IMAGE=kodi-switch2-devkit
JOBS="${JOBS:-$(nproc)}"
# Build and package directories: release in build/ + out/ (defaults); a diagnostics build can live
# beside it, e.g. BUILD_DIR=build-diag OUT_DIR=out-diag SWITCH2_DIAG=ON ./build.sh configure build nro.
BUILD_DIR="${BUILD_DIR:-build}"
OUT_DIR="${OUT_DIR:-out}"
run() { podman run --rm --security-opt label=disable -v "$HERE:/k" -w /k "$IMAGE" bash -lc "$*"; }

hosttools() {
  # The flatbuffers source is fetched on the host: untarring into the bind mount from inside the container fails.
  mkdir -p "$HERE/hosttools/src"
  if [ ! -d "$HERE/hosttools/src/flatbuffers-23.3.3" ]; then
    curl -sL -o "$HERE/hosttools/src/fb.tgz" https://github.com/google/flatbuffers/archive/v23.3.3.tar.gz
    tar -C "$HERE/hosttools/src" -xzf "$HERE/hosttools/src/fb.tgz"
  fi
  run "set -e
       cmake -S xbmc/tools/depends/native/TexturePacker/src -B hosttools/build/TexturePacker -DCMAKE_BUILD_TYPE=Release -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
             -DKODI_SOURCE_DIR=/k/xbmc -DCMAKE_INSTALL_PREFIX=/k/hosttools -DCMAKE_CXX_FLAGS='-DTARGET_POSIX -DTARGET_LINUX' >/dev/null
       cmake --build hosttools/build/TexturePacker -j$JOBS && cmake --install hosttools/build/TexturePacker >/dev/null
       cmake -S xbmc/tools/depends/native/JsonSchemaBuilder/src -B hosttools/build/JsonSchemaBuilder -DCMAKE_BUILD_TYPE=Release -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
             -DCMAKE_INSTALL_PREFIX=/k/hosttools >/dev/null
       cmake --build hosttools/build/JsonSchemaBuilder -j$JOBS && cmake --install hosttools/build/JsonSchemaBuilder >/dev/null
       cmake -S hosttools/src/flatbuffers-23.3.3 -B hosttools/build/flatc -DCMAKE_BUILD_TYPE=Release -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
             -DFLATBUFFERS_BUILD_TESTS=OFF -DFLATBUFFERS_BUILD_FLATLIB=OFF >/dev/null
       cmake --build hosttools/build/flatc -j$JOBS --target flatc && install -m755 hosttools/build/flatc/flatc hosttools/bin/flatc
       ls -l hosttools/bin"
}

# Groovy + Apache Commons for Kodi's Python binding generator (xbmc/interfaces/swig/CMakeLists.txt). Fetched and
# unpacked on the host into the layout that CMakeLists checks first (prefix/share/groovy, share/java/{lang,text}),
# so configure needs no FetchContent download inside the container. Hashes are Kodi's own.
pyjars() {
  local S="$HERE/deps/src-extra" P="$HERE/prefix/share" M=http://mirrors.kodi.tv/build-deps/sources
  mkdir -p "$S" "$P/java"
  get() { # get <file> <sha512>
    [ -f "$S/$1" ] || curl -sfL -o "$S/$1" "$M/$1"
    echo "$2  $S/$1" | sha512sum -c --status || { echo "sha512 mismatch: $S/$1"; exit 1; }
  }
  get apache-groovy-binary-4.0.26.zip 60d1aea85008a438a9498178dc2137385653f1ea294e7e257959e9e0f94382e160e217e1090047c2a944e328cf46e8ebcfc4b015f7d0cb9f2fcf9d880c6c1344
  get commons-lang3-3.17.0-bin.tar.gz 8927a406a1bd084b548f89cf15fe4bc7567dcaa50ae8abae54e9c883c1574c648fcc8ad6c3abaa381dfbdf1801727ac5e6c4572063203a1dfac293d122282f05
  get commons-text-1.13.0-bin.tar.gz a51667463e88b2d017c3baebb9bbe42c4e50cba96ffe19c89680e82002a4db8b9007daff75f8dbf8bf3725be13a3f03f3c2bc4e1f11e8e02dcab4408abdb44a1
  if [ ! -f "$P/groovy/lib/groovy-4.0.26.jar" ]; then
    rm -rf "$P/groovy" "$S/groovy-4.0.26"; unzip -q -d "$S" "$S/apache-groovy-binary-4.0.26.zip"; mv "$S/groovy-4.0.26" "$P/groovy"
  fi
  if [ ! -f "$P/java/lang/commons-lang3-3.17.0.jar" ]; then
    rm -rf "$P/java/lang"; mkdir -p "$P/java/lang"; tar -C "$P/java/lang" --strip-components=1 -xzf "$S/commons-lang3-3.17.0-bin.tar.gz"
  fi
  if [ ! -f "$P/java/text/commons-text-1.13.0.jar" ]; then
    rm -rf "$P/java/text"; mkdir -p "$P/java/text"; tar -C "$P/java/text" --strip-components=1 -xzf "$S/commons-text-1.13.0-bin.tar.gz"
  fi
  ls "$P/groovy/lib/groovy-4.0.26.jar" "$P/java/lang/commons-lang3-3.17.0.jar" "$P/java/text/commons-text-1.13.0.jar"
}

deps() {
  pyjars
  mkdir -p "$HERE/deps/src-extra"
  [ -f "$HERE/deps/src-extra/fstrcmp-0.7.D001.tar.gz" ] || curl -sfL -o "$HERE/deps/src-extra/fstrcmp-0.7.D001.tar.gz" http://mirrors.kodi.tv/build-deps/sources/fstrcmp-0.7.D001.tar.gz
  run ". /opt/devkitpro/switch2vars.sh; bash deps/build-deps.sh && bash deps/build-extra.sh"
}

configure() {
  run "set -e; cmake -S xbmc -B $BUILD_DIR -G Ninja \
      -DCMAKE_TOOLCHAIN_FILE=/k/switch2-kodi.cmake \
      -DCMAKE_BUILD_TYPE=Release -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
      -DCORE_SYSTEM_NAME=switch2 -DCORE_PLATFORM_NAME=switch2 -DAPP_RENDER_SYSTEM=gles \
      -DDEPENDS_PATH=/k/prefix -DNATIVEPREFIX=/k/hosttools \
      -DWITH_TEXTUREPACKER=/k/hosttools/bin/TexturePacker -DWITH_JSONSCHEMABUILDER=/k/hosttools/bin/JsonSchemaBuilder \
      -DFLATBUFFERS_FLATC_EXECUTABLE=/k/hosttools/bin/flatc \
      -DFFMPEG_PATH=/k/prefix -DENABLE_INTERNAL_FFMPEG=OFF \
      -DENABLE_INTERNAL_FMT=ON -DENABLE_INTERNAL_SPDLOG=ON -DENABLE_INTERNAL_FLATBUFFERS=ON \
      -DENABLE_INTERNAL_RapidJSON=ON -DENABLE_INTERNAL_TAGLIB=ON -DENABLE_INTERNAL_PCRE=ON \
      -DENABLE_INTERNAL_CROSSGUID=ON -DENABLE_INTERNAL_FSTRCMP=OFF -DENABLE_INTERNAL_KISSFFT=ON \
      -DENABLE_INTERNAL_TINYXML2=ON -DENABLE_INTERNAL_DAV1D=OFF -DENABLE_INTERNAL_UDFREAD=OFF \
      -DENABLE_SWITCH2_DIAG=${SWITCH2_DIAG:-OFF} \
      -DENABLE_PYTHON=ON -DENABLE_UPNP=OFF -DENABLE_AIRTUNES=OFF -DENABLE_OPTICAL=OFF \
      -DENABLE_DVDCSS=OFF -DENABLE_TESTING=OFF -DENABLE_CCACHE=OFF \
      -DENABLE_ALSA=OFF -DENABLE_PULSEAUDIO=OFF -DENABLE_PIPEWIRE=OFF -DENABLE_SNDIO=OFF \
      -DENABLE_AVAHI=OFF -DENABLE_BLUETOOTH=OFF -DENABLE_BLURAY=OFF -DENABLE_CAP=OFF -DENABLE_CEC=OFF \
      -DENABLE_DBUS=OFF -DENABLE_ISO9660PP=OFF -DENABLE_LCMS2=OFF -DENABLE_LIRCCLIENT=OFF \
      -DENABLE_MDNS=OFF -DENABLE_MICROHTTPD=ON -DENABLE_NFS=ON -DENABLE_PLIST=OFF \
      -DENABLE_SMBCLIENT=OFF -DENABLE_UDEV=OFF -DENABLE_UDFREAD=OFF -DENABLE_XSLT=OFF \
      -DENABLE_MARIADBCLIENT=OFF -DENABLE_MYSQLCLIENT=OFF -DENABLE_DAV1D=OFF \
      2>&1 | tee $BUILD_DIR-configure.log | tail -60"
}

# Kodi's internal spdlog/pcre/libdvdread/libdvdnav install into the shared prefix/. After a build in a
# second directory (diagnostics), re-install the release build's copies so build/ stays byte-identical.
INTERNAL_PREFIX_DEPS="spdlog pcre libdvdread libdvdnav"
restore_prefix() {
  [ "$BUILD_DIR" != build ] && [ -f "$HERE/build/build.ninja" ] || return 0
  local d
  for d in $INTERNAL_PREFIX_DEPS; do rm -f "$HERE/build/build/$d/src/$d-stamp/$d-install" "$HERE/build/build/$d/src/$d-stamp/$d-done"; done
  run "cmake --build build --target $INTERNAL_PREFIX_DEPS >/dev/null && echo 'prefix: release copies of $INTERNAL_PREFIX_DEPS re-installed'"
}
build() { run "cmake --build $BUILD_DIR -j$JOBS -- -k 0 2>&1 | tee $BUILD_DIR-compile.log | grep -E 'error|FAILED' | head -80; tail -3 $BUILD_DIR-compile.log"; restore_prefix; }

nro() { run "BUILD_DIR=$BUILD_DIR OUT_DIR=$OUT_DIR bash /k/package.sh"; }

[ $# -eq 0 ] && set -- all
for step in "$@"; do
  case "$step" in
    hosttools|deps|pyjars|configure|build|nro) "$step" ;;
    all) hosttools; deps; configure; build; nro ;;
    *) echo "usage: $0 {hosttools|deps|pyjars|configure|build|nro|all}..."; exit 2 ;;
  esac
done
