#!/usr/bin/env bash
# Kodi for Switch 2 — build driver. Run on the host; every step runs in the kodi-switch2-devkit image.
#   ./build.sh hosttools   native TexturePacker + JsonSchemaBuilder + flatc  -> hosttools/bin
#   ./build.sh deps        static libs the devkit lacks                     -> prefix/
#   ./build.sh configure   cmake configure of xbmc/ for switch2             -> build/
#   ./build.sh build       compile + link kodi.elf
#   ./build.sh nro         elf2nro + data tree                              -> out/
#   ./build.sh all
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
IMAGE=kodi-switch2-devkit
JOBS="${JOBS:-$(nproc)}"
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

deps() {
  mkdir -p "$HERE/deps/src-extra"
  [ -f "$HERE/deps/src-extra/fstrcmp-0.7.D001.tar.gz" ] || curl -sfL -o "$HERE/deps/src-extra/fstrcmp-0.7.D001.tar.gz" http://mirrors.kodi.tv/build-deps/sources/fstrcmp-0.7.D001.tar.gz
  run ". /opt/devkitpro/switch2vars.sh; bash deps/build-deps.sh && bash deps/build-extra.sh"
}

configure() {
  run "set -e; cmake -S xbmc -B build -G Ninja \
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
      -DENABLE_PYTHON=OFF -DENABLE_UPNP=OFF -DENABLE_AIRTUNES=OFF -DENABLE_OPTICAL=OFF \
      -DENABLE_DVDCSS=OFF -DENABLE_TESTING=OFF -DENABLE_CCACHE=OFF \
      -DENABLE_ALSA=OFF -DENABLE_PULSEAUDIO=OFF -DENABLE_PIPEWIRE=OFF -DENABLE_SNDIO=OFF \
      -DENABLE_AVAHI=OFF -DENABLE_BLUETOOTH=OFF -DENABLE_BLURAY=OFF -DENABLE_CAP=OFF -DENABLE_CEC=OFF \
      -DENABLE_DBUS=OFF -DENABLE_ISO9660PP=OFF -DENABLE_LCMS2=OFF -DENABLE_LIRCCLIENT=OFF \
      -DENABLE_MDNS=OFF -DENABLE_MICROHTTPD=OFF -DENABLE_NFS=OFF -DENABLE_PLIST=OFF \
      -DENABLE_SMBCLIENT=OFF -DENABLE_UDEV=OFF -DENABLE_UDFREAD=OFF -DENABLE_XSLT=OFF \
      -DENABLE_MARIADBCLIENT=OFF -DENABLE_MYSQLCLIENT=OFF -DENABLE_DAV1D=OFF \
      2>&1 | tee build-configure.log | tail -60"
}

build() { run "cmake --build build -j$JOBS -- -k 0 2>&1 | tee build-compile.log | grep -E 'error|FAILED' | head -80; tail -3 build-compile.log"; }

nro() { run "bash /k/package.sh"; }

case "${1:-all}" in
  hosttools|deps|configure|build|nro) "$1" ;;
  all) hosttools; deps; configure; build; nro ;;
  *) echo "usage: $0 {hosttools|deps|configure|build|nro|all}"; exit 2 ;;
esac
