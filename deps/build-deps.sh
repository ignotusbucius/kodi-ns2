#!/usr/bin/env bash
# build-deps.sh: cross-build the static libraries Kodi 21.3 needs that the
# Switch 2 devkit (ounce-devkit image) lacks, into ./prefix.
#
# Run inside the ounce-devkit image, after sourcing switch2vars.sh:
#   podman run --rm --security-opt label=disable -v <repo>:/k \
#     -w /k ounce-devkit bash -lc '. /opt/devkitpro/switch2vars.sh; bash deps/build-deps.sh'
# (label=disable, not :Z: another container relabelling the tree with :Z
# mid-build made every file "Permission denied" to this one.)
#
# Idempotent: a library whose stamp prefix/.stamp-<name> exists is skipped;
# delete the stamp to rebuild it. Tarballs are cached in deps/src/ and checked
# against the sha256 pinned below; each build runs in a fresh deps/build/<name>
# with its log in deps/build/<name>.log. Optional arguments name the libraries
# to build (default: all, in order). Installed .pc files written by upstream
# build systems carry prefix=/k/prefix (the container path).
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
KODI="$(dirname "$HERE")"
PREFIX="$KODI/prefix"
SRC="$HERE/src"
BUILD="$HERE/build"
PATCHES="$HERE/patches"
HOST=aarch64-none-elf
JOBS="${JOBS:-$(nproc)}"

: "${DEVKITPRO:?source /opt/devkitpro/switch2vars.sh first}"
: "${PORTLIBS_PREFIX:?source /opt/devkitpro/switch2vars.sh first}"
command -v "$HOST-gcc" >/dev/null || { echo "no $HOST-gcc on PATH" >&2; exit 1; }
SPECS="$DEVKITPRO/libounce/switch.specs"

mkdir -p "$SRC" "$BUILD" "$PREFIX/lib/pkgconfig" "$PREFIX/include"

# Toolchain. The prefix comes first so its headers (iconv.h) shadow newlib's.
export CC="$HOST-gcc" CXX="$HOST-g++" AR="$HOST-gcc-ar" RANLIB="$HOST-gcc-ranlib"
export NM="$HOST-gcc-nm" STRIP="$HOST-strip" LD="$HOST-ld"
export CPPFLAGS="-I$PREFIX/include $CPPFLAGS"
export LDFLAGS="-L$PREFIX/lib $LDFLAGS -specs=$SPECS"
export LIBS="${LIBS:--lounce}"
# Only our prefix is visible to pkg-config (never the Switch 1 FFmpeg 7.1 .pc).
export PKG_CONFIG_LIBDIR="$PREFIX/lib/pkgconfig"
unset PKG_CONFIG_PATH PKG_CONFIG_SYSROOT_DIR || true

# name|version|url|sha256
TARBALLS=(
  "libiconv|1.17|https://ftp.gnu.org/pub/gnu/libiconv/libiconv-1.17.tar.gz|8f74213b56238c85a50a5329f77e06198771e70dd9a739779f4c02f65d971313"
  "openssl|3.0.22|https://github.com/openssl/openssl/releases/download/openssl-3.0.22/openssl-3.0.22.tar.gz|67ebca7e50d17383028045486653492195b83db95f8558709701bb47b5c1ef81"
  "sqlite3|3.49.2|https://www.sqlite.org/2025/sqlite-autoconf-3490200.tar.gz|5c6d8697e8a32a1512a9be5ad2b2e7a891241c334f56f8b0fb4fc6051e1652e8"
  "tinyxml|2.6.2_2|http://mirrors.kodi.tv/build-deps/sources/tinyxml-2.6.2_2.tar.gz|8164c9ad48b9028667768a584d62f7760cfbfb90d0dd6214ad174403058da10c"
  "libcdio|2.1.0|https://ftp.gnu.org/gnu/libcdio/libcdio-2.1.0.tar.bz2|8550e9589dbd594bfac93b81ecf129b1dc9d0d51e90f9696f1b2f9b2af32712b"
  "ffmpeg|6.0.1|http://mirrors.kodi.tv/build-deps/sources/ffmpeg-6.0.1.tar.gz|375fd8abab657d18578554927d23abfc9cb3b6794bd9839330230cf5f9fcea26"
)
ORDER=(libiconv openssl sqlite3 tinyxml libcdio ffmpeg)

# has_sym <lib.a> <symbol>: the archive defines the symbol (no grep -q: SIGPIPE
# under pipefail). C++ names are matched demangled.
has_sym() {
  "$NM" -C --defined-only "$PREFIX/lib/$1" 2>/dev/null | awk -v s="$2" '
    { $1=""; $2=""; sub(/^  /, ""); if ($0 == s) f=1 } END { exit !f }'
}

log() { printf '== [%s] %s\n' "$1" "$2"; }

field() { # field <name> <index>
  local e
  for e in "${TARBALLS[@]}"; do
    IFS='|' read -r n v u s <<<"$e"
    if [ "$n" = "$1" ]; then
      case "$2" in 1) echo "$v";; 2) echo "$u";; 3) echo "$s";; esac
      return 0
    fi
  done
  return 1
}

fetch() { # fetch <name> -> prints tarball path
  local url sum f
  url="$(field "$1" 2)"; sum="$(field "$1" 3)"; f="$SRC/$(basename "$url")"
  if [ ! -f "$f" ] || ! echo "$sum  $f" | sha256sum -c --status; then
    log "$1" "download $url" >&2
    curl -LfsS --retry 3 -o "$f.part" "$url"
    mv "$f.part" "$f"
  fi
  if ! echo "$sum  $f" | sha256sum -c --status; then
    echo "sha256 mismatch for $f (want $sum)" >&2; exit 1
  fi
  echo "$f"
}

unpack() { # unpack <name> -> fresh build dir, prints it
  local tb d
  tb="$(fetch "$1")"; d="$BUILD/$1"
  rm -rf "$d"; mkdir -p "$d"
  tar --no-same-owner -xf "$tb" -C "$d" --strip-components=1
  echo "$d"
}

# ---------------------------------------------------------------- libiconv
build_libiconv() {
  local d; d="$(unpack libiconv)"; cd "$d"
  # GCC 15 defaults to C23, where libiconv 1.17's K&R `extern size_t mbrtowc ();`
  # is a zero-argument prototype: build as gnu17.
  CFLAGS="$CFLAGS -std=gnu17" ./configure --host="$HOST" --prefix="$PREFIX" \
    --disable-shared --enable-static --enable-extra-encodings --disable-nls
  # Only the libraries and headers: the iconv program is not needed.
  make lib/localcharset.h
  make -j"$JOBS" -C lib
  make -C libcharset install
  make -C lib install
  install -m644 include/iconv.h.inst "$PREFIX/include/iconv.h"
  rm -f "$PREFIX"/lib/*.la
  # iconv.h maps iconv_open -> libiconv_open; Kodi's FindIconv links -liconv.
  has_sym libiconv.a libiconv_open
}

# ---------------------------------------------------------------- openssl
# Patch patches/openssl/0001-switch2-horizon.patch (each hunk on __SWITCH__):
#  rand_unix.c  seed from libounce randomGet(): newlib getentropy() has no
#               _getentropy_r backend and there is no /dev/urandom;
#  armcap.c     fixed Cortex-A78C caps (NEON/AES/PMULL/SHA1/SHA256): no
#               getauxval() and SIGILL probing would kill the process;
#  uid.c        OPENSSL_issetugid() = 0: no getuid/geteuid in newlib/libounce;
#  sockets.h    #undef AF_UNIX: libounce defines it but has no sockaddr_un.
# Defines: -DNO_SYS_UN_H -DOPENSSL_NO_UNIX_SOCK (no sys/un.h), -DNO_SYSLOG.
OPENSSL_CONF_FLAGS=(linux-aarch64 no-shared no-dso no-async no-afalgeng no-engine
  no-tests no-ui-console no-secure-memory -DNO_SYS_UN_H -DOPENSSL_NO_UNIX_SOCK -DNO_SYSLOG)
build_openssl() {
  local d; d="$(unpack openssl)"; cd "$d"
  patch -p1 -i "$PATCHES/openssl/0001-switch2-horizon.patch"
  local asm=()
  [ "${OPENSSL_NO_ASM:-0}" = 1 ] && asm=(no-asm)
  # Configure takes CC/CFLAGS/CPPFLAGS/LDFLAGS from the environment.
  ./Configure "${OPENSSL_CONF_FLAGS[@]}" "${asm[@]}" \
    --prefix="$PREFIX" --libdir=lib --openssldir="$PREFIX/ssl"
  # The apps library needs syslog.h and is not installed: drop it (as Kodi's
  # recipe drops PROGRAMS).
  sed -i -e 's|^LIBS=apps/libapps.a |LIBS=|' -e 's|^PROGRAMS=.*|PROGRAMS=|' Makefile
  make -j"$JOBS" build_libs
  make install_dev
  has_sym libssl.a SSL_CTX_new
  has_sym libcrypto.a EVP_DigestInit_ex
}
# ---------------------------------------------------------------- sqlite3
# The amalgamation, compiled directly. Kodi's recipe adds COLUMN_METADATA and
# TEMP_STORE=3 (kept) and an mmap size (dropped: newlib has no mmap, so
# SQLITE_MAX_MMAP_SIZE=0, and WAL, which needs mmap'd shared memory, is omitted).
SQLITE_DEFS=(-DSQLITE_DEFAULT_UNIX_VFS=\"unix-dotfile\" -DSQLITE_OMIT_LOAD_EXTENSION -DSQLITE_THREADSAFE=1
  -DSQLITE_ENABLE_COLUMN_METADATA=1 -DSQLITE_TEMP_STORE=3 -DSQLITE_MAX_MMAP_SIZE=0
  -DSQLITE_OMIT_WAL -DHAVE_USLEEP=1 -DHAVE_LOCALTIME_R=1 -DHAVE_GMTIME_R=1
  -DHAVE_STRERROR_R=1 -DHAVE_STDINT_H=1 -DHAVE_INTTYPES_H=1)
build_sqlite3() {
  local d; d="$(unpack sqlite3)"; cd "$d"
  # patches/sqlite3/0001-no-fchown.patch: no HAVE_FCHOWN on __SWITCH__.
  patch -p1 -i "$PATCHES/sqlite3/0001-no-fchown.patch"
  # shellcheck disable=SC2086
  "$CC" $CPPFLAGS $CFLAGS "${SQLITE_DEFS[@]}" -c sqlite3.c -o sqlite3.o
  rm -f libsqlite3.a; "$AR" rcs libsqlite3.a sqlite3.o
  install -m644 libsqlite3.a "$PREFIX/lib/"
  install -m644 sqlite3.h sqlite3ext.h "$PREFIX/include/"
  cat >"$PREFIX/lib/pkgconfig/sqlite3.pc" <<PC
prefix=\${pcfiledir}/../..
exec_prefix=\${prefix}
libdir=\${prefix}/lib
includedir=\${prefix}/include

Name: SQLite
Description: SQL database engine
Version: $(field sqlite3 1)
Libs: -L\${libdir} -lsqlite3
Libs.private: -lm
Cflags: -I\${includedir}
PC
  has_sym libsqlite3.a sqlite3_open_v2
}
# ---------------------------------------------------------------- tinyxml
# Kodi's tinyxml-2.6.2_2 tarball (TinyXML v1; its tinyxml.h already defines
# TIXML_USE_STL). Kodi's recipe runs autoreconf, which the image lacks, so the
# four sources of src/Makefile.am are compiled directly with the same define.
build_tinyxml() {
  local d f; d="$(unpack tinyxml)"; cd "$d"
  for f in tinystr tinyxml tinyxmlerror tinyxmlparser; do
    # shellcheck disable=SC2086
    "$CXX" $CPPFLAGS $CXXFLAGS -DTIXML_USE_STL -Iinclude -c "src/$f.cpp" -o "src/$f.o"
  done
  rm -f libtinyxml.a; "$AR" rcs libtinyxml.a src/*.o
  install -m644 libtinyxml.a "$PREFIX/lib/"
  install -m644 include/tinyxml.h include/tinystr.h "$PREFIX/include/"
  cat >"$PREFIX/lib/pkgconfig/tinyxml.pc" <<PC
prefix=\${pcfiledir}/../..
exec_prefix=\${prefix}
libdir=\${prefix}/lib
includedir=\${prefix}/include

Name: TinyXml
Description: simple, small, C++ XML parser
Version: 2.6.2
Libs: -L\${libdir} -ltinyxml
Cflags: -I\${includedir} -DTIXML_USE_STL
PC
  has_sym libtinyxml.a 'TiXmlDocument::LoadFile(char const*, TiXmlEncoding)'
}
# ---------------------------------------------------------------- libcdio
# Kodi pins 0.80 with configure/cross patches; those do not apply to 2.1.0.
# Joliet stays on: it uses iconv from step 1 (prefix/lib/libiconv.a).
LIBCDIO_CONF_FLAGS=(--disable-shared --enable-static --disable-cxx --disable-cpp-progs
  --disable-example-progs --without-cd-drive --without-cd-info --without-cdda-player
  --without-cd-read --without-iso-info --without-iso-read --disable-vcd-info
  --without-versioned-libs --enable-joliet --with-libiconv-prefix="$PREFIX")
build_libcdio() {
  local d; d="$(unpack libcdio)"; cd "$d"
  # newlib ships glob.h but no glob()/globfree(): treat the header as absent so
  # the image drivers' device listing takes its no-glob branch.
  ac_cv_header_glob_h=no \
    ./configure --host="$HOST" --prefix="$PREFIX" "${LIBCDIO_CONF_FLAGS[@]}"
  make -j"$JOBS" -C include
  make -j"$JOBS" -C lib
  make -C include install
  make -C lib install
  make install-pkgconfigDATA 2>/dev/null || install -m644 libcdio.pc libiso9660.pc "$PREFIX/lib/pkgconfig/"
  rm -f "$PREFIX"/lib/*.la
  has_sym libcdio.a cdio_open
  has_sym libiso9660.a iso9660_open
}
# ---------------------------------------------------------------- ffmpeg
# Kodi's pinned 6.0.1 (its FFMPEG-VERSION and mirror tarball; that recipe
# directory has no patches). Flags follow Kodi's ffmpeg CMakeLists.txt (gpl,
# postproc, pthreads, runtime-cpudetect, no sdl2/vulkan/doc/programs/devices)
# plus --disable-autodetect so nothing from the Switch 1 portlibs (bz2, lzma,
# dav1d...) is picked up; only zlib is enabled. Never the portlibs' FFmpeg 7.1.
FFMPEG_NETWORK="${FFMPEG_NETWORK:---enable-network}"
build_ffmpeg() {
  local d; d="$(unpack ffmpeg)"; cd "$d"
  # patches/ffmpeg/0001-tcp-no-maxseg.patch: skip TCP_MAXSEG when undefined.
  patch -p1 -i "$PATCHES/ffmpeg/0001-tcp-no-maxseg.patch"
  # patches/ffmpeg/0002-none-data-rel-ro.patch: target-os=none puts relocated
  # asm constants in .data.rel.ro (else "read-only segment has dynamic
  # relocations" under libounce's -z text PIE link).
  patch -p1 -i "$PATCHES/ffmpeg/0002-none-data-rel-ro.patch"
  ./configure --prefix="$PREFIX" --enable-cross-compile --arch=aarch64 --target-os=none \
    --cross-prefix="$HOST-" --cc="$CC" --cxx="$CXX" --ar="$AR" --ranlib="$RANLIB" \
    --nm="$HOST-nm" --strip="$STRIP" --pkg-config=pkg-config --pkg-config-flags=--static \
    --disable-shared --enable-static --enable-pic --disable-programs --disable-doc \
    --disable-debug --enable-gpl --enable-postproc --enable-runtime-cpudetect \
    --enable-pthreads "$FFMPEG_NETWORK" --disable-devices --disable-hwaccels \
    --disable-autodetect --enable-zlib --disable-sdl2 --disable-vulkan \
    --extra-version="kodi-6.0.1" --extra-libs="$LIBS"
  # (configure already reads CFLAGS/CPPFLAGS/LDFLAGS from the environment.)
  make -j"$JOBS"
  make install
  has_sym libavcodec.a avcodec_send_packet
  has_sym libavformat.a avformat_open_input
}

# ---------------------------------------------------------------- driver
want=("$@"); [ ${#want[@]} -eq 0 ] && want=("${ORDER[@]}")
failed=()
for name in "${ORDER[@]}"; do
  skip=1; for w in "${want[@]}"; do [ "$w" = "$name" ] && skip=0; done
  [ $skip -eq 1 ] && continue
  if [ -f "$PREFIX/.stamp-$name" ]; then log "$name" "stamp present, skipped"; continue; fi
  log "$name" "building $(field "$name" 1)"
  # Not in an `if`: errexit must stay active inside the build subshell.
  set +e; ( set -euo pipefail; "build_$name" ) >"$BUILD/$name.log" 2>&1; rc=$?; set -e
  if [ $rc -eq 0 ]; then
    printf '%s %s\n' "$(field "$name" 1)" "$(field "$name" 3)" >"$PREFIX/.stamp-$name"
    log "$name" "done"
  else
    log "$name" "FAILED, last lines of $BUILD/$name.log:"; tail -n 25 "$BUILD/$name.log"
    failed+=("$name")
  fi
done
if [ ${#failed[@]} -gt 0 ]; then echo "FAILED: ${failed[*]}"; exit 1; fi
echo "all requested libraries present in $PREFIX"
