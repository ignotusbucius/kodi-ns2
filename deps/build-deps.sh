#!/usr/bin/env bash
# build-deps.sh: cross-build the static libraries Kodi 21.3 needs that the
# Switch 2 devkit (ounce-devkit image) lacks, into homebrew/kodi/prefix.
#
# Run inside the ounce-devkit image, after sourcing switch2vars.sh:
#   podman run --rm --security-opt label=disable -v <repo>/homebrew/kodi:/k \
#     -w /k ounce-devkit bash -lc '. /opt/devkitpro/switch2vars.sh; bash deps/build-deps.sh'
# (label=disable, not :Z: another container relabelling homebrew/kodi with :Z
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
  "dav1d|1.4.1|http://mirrors.kodi.tv/build-deps/sources/dav1d-1.4.1.tar.bz2|ab02c6c72c69b2b24726251f028b7cb57d5b3659eeec9f67f6cecb2322b127d8"
  "ffmpeg|6.0.1|http://mirrors.kodi.tv/build-deps/sources/ffmpeg-6.0.1.tar.gz|375fd8abab657d18578554927d23abfc9cb3b6794bd9839330230cf5f9fcea26"
  "libnfs|5.0.2|http://mirrors.kodi.tv/build-deps/sources/libnfs-5.0.2.tar.gz|637e56643b19da9fba98f06847788c4dad308b723156a64748041035dcdf9bd3"
  "libmicrohttpd|0.9.77|http://mirrors.kodi.tv/build-deps/sources/libmicrohttpd-0.9.77.tar.gz|9e7023a151120060d2806a6ea4c13ca9933ece4eacfc5c9464d20edddb76b0a0"
  "python3|3.11.7|https://www.python.org/ftp/python/3.11.7/Python-3.11.7.tar.xz|18e1aa7e66ff3a58423d59ed22815a6954e53342122c45df20c96877c062b9b7"
  "pillow|8.4.0|https://files.pythonhosted.org/packages/7d/2a/2fc11b54e2742db06297f7fa7f420a0e3069fdcf0e4b57dfec33f0b08622/Pillow-8.4.0.tar.gz|b8e2f83c56e141920c39464b852de3719dfbfb6e3c99a2d8da0edf4fb33176ed"
)
ORDER=(libiconv openssl sqlite3 tinyxml libcdio dav1d ffmpeg python3 libnfs libmicrohttpd)

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
  # patches/openssl/0002-switch2-crumbs.patch: S2CRUMB() breadcrumbs on the method-fetch path
  # (core_algorithm.c, core_fetch.c, evp_fetch.c, digest.c, provider_core.c) through a WEAK
  # Switch2OsslCrumb (include/internal/switch2_crumb.h): no-ops unless Kodi defines it.
  patch -p1 -i "$PATCHES/openssl/0002-switch2-crumbs.patch"
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
  # patches/sqlite3/0002-switch2-shared-cache-close-race.patch (rev 34, __SWITCH__ only): the FS
  # refuses a second open() (EIO) of a file open for writing. sqlite3BtreeClose holds
  # SQLITE_MUTEX_STATIC_OPEN from dropping a shared cache's last reference until its file is closed
  # (a concurrent sqlite3BtreeOpen no longer misses the cache and opens a half-closed file);
  # unixOpen retries an EIO open for ~1 s and never falls back to read-only on EIO (rev 33d crash
  # "MyMusic83.db is read only").
  patch -p1 -i "$PATCHES/sqlite3/0002-switch2-shared-cache-close-race.patch"
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
# ---------------------------------------------------------------- dav1d
# Kodi 21's pinned 1.4.1 (tools/depends/target/dav1d; its sha512 matches), built like Kodi's recipe
# (meson, static, release, asm on, no tools/examples/tests) with a cross file for libounce. AArch64
# NEON asm; CPU features: dav1d's "unsupported OS" path enables NEON only (no getauxval here).
# patches/dav1d/0001-switch2-cpu-count.patch: dav1d_num_logical_processors returns 3 on __SWITCH__
# (no sysconf in newlib/libounce; FFmpeg passes Kodi's thread count anyway).
build_dav1d() {
  local d; d="$(unpack dav1d)"; cd "$d"
  patch -p1 -i "$PATCHES/dav1d/0001-switch2-cpu-count.patch"
  local q; q() { local a out=""; for a in "$@"; do out="$out'$a', "; done; echo "[${out%, }]"; }
  # shellcheck disable=SC2086
  cat >cross.meson <<CROSS
[binaries]
c = '$CC'
cpp = '$CXX'
ar = '$AR'
strip = '$STRIP'
nm = '$NM'
pkgconfig = 'false'

[built-in options]
c_args = $(q $CPPFLAGS $CFLAGS)
c_link_args = $(q $LDFLAGS $LIBS)

[host_machine]
system = 'switch2'
cpu_family = 'aarch64'
cpu = 'cortex-a78c'
endian = 'little'
CROSS
  env -u CC -u CXX -u CFLAGS -u CPPFLAGS -u LDFLAGS -u LIBS \
    meson setup build --cross-file cross.meson --prefix="$PREFIX" --libdir=lib --buildtype=release \
    --default-library=static -Denable_asm=true -Denable_tools=false -Denable_examples=false \
    -Denable_tests=false
  ninja -C build -j"$JOBS"
  ninja -C build install
  has_sym libdav1d.a dav1d_open
  has_sym libdav1d.a dav1d_send_data
}
# ---------------------------------------------------------------- ffmpeg
# Kodi's pinned 6.0.1 (its FFMPEG-VERSION and mirror tarball; that recipe
# directory has no patches). Flags follow Kodi's ffmpeg CMakeLists.txt (gpl,
# postproc, pthreads, runtime-cpudetect, no sdl2/vulkan/doc/programs/devices)
# plus --disable-autodetect so nothing from the Switch 1 portlibs (bz2, lzma,
# dav1d...) is picked up; only zlib and our own prefix/ libdav1d (rev 26) are enabled. Never the
# portlibs' FFmpeg 7.1. The native av1 decoder stays: Kodi opens it by name first (hwaccel probe, one
# thread); with no AV1 hwaccel its get_format sets STATE_HW_FAILED, Kodi reopens, and
# avcodec_find_decoder(AV_CODEC_ID_AV1) then returns libdav1d (listed before av1 in allcodecs.c)
# with frame threads (STATE_SW_MULTI). Without av1, libdav1d would be opened single-threaded.
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
    --disable-autodetect --enable-zlib --enable-libdav1d --disable-sdl2 --disable-vulkan \
    --extra-version="kodi-6.0.1" --extra-libs="$LIBS"
  # (configure already reads CFLAGS/CPPFLAGS/LDFLAGS from the environment.)
  make -j"$JOBS"
  make install
  has_sym libavcodec.a avcodec_send_packet
  has_sym libavcodec.a ff_libdav1d_decoder
  has_sym libavformat.a avformat_open_input
}

# ---------------------------------------------------------------- python3
# CPython 3.11.7 (Kodi's PYTHON3-VERSION; its sha512 is checked too), embedded in kodi.nro for Python
# add-ons. Static only (no dlopen): MODULE_BUILDTYPE=static compiles every detected extension module
# into libpython3.11.a and Modules/config.c (_PyImport_Inittab), as Kodi's depends recipe does for
# every platform. A native 3.11.7 is built first for --with-build-python (freezing, sysconfigdata,
# compileall). Patches in patches/python3/ (all on __SWITCH__ or the switch2 sys.platform):
#  0001 configure    accept --host=aarch64-*-elf as ac_sys_system=Switch2 (sys.platform "switch2");
#  0002 urandom      os.urandom/hash seed from libounce randomGet() (no getentropy backend, no /dev/urandom);
#  0003 pyport.h     UTF-8 locale + filesystem encoding (newlib's C locale is ASCII), as Android/VxWorks;
#  0004 threads      _POSIX_THREAD_ATTR_STACKSIZE for newlib + 768 KiB default stack for Python threads (rev 35;
#                    before, newlib lacked the macro: Python threads had NULL attrs = the 1 MiB wrapper default);
#  0005 subprocess   switch2 has no processes: Popen raises OSError(ENOTSUP), like emscripten/wasi;
#  0006 posix        os.getcwd() drops the "sdmc:" device prefix (posixpath needs "/..." absolute).
#  0007 pyport.h     PY_SSIZE_T_MAX without SSIZE_MAX (newlib <limits.h> lacks it);
#  0008 time         newlib's _timezone/_daylight (no timezone/daylight);
#  0009 posix        os.statvfs() without fstatvfs() (libsysbase has statvfs only);
#  0010 stubs        Modules/switch2_compat.c: empty passwd/group database for pwd/grp;
#                    Modules/_posixsubprocess_switch2.c: importable, fork_exec() -> OSError(ENOSYS);
#  0011 _sqlite      bounded retry on SQLITE_LOCKED[_SHAREDCACHE] (Kodi's shared cache; busy_timeout
#                    covers only SQLITE_BUSY): prepare and first step, 1->50 ms backoff, ~2 s (util.h).
#  0012 fileutils    set_inheritable() is a no-op (ioctl(FIOCLEX) is socket-only on the console);
#  0013 config.c.in  inittab entries "PIL._imaging", "PIL._imagingft", "PIL._imagingmath",
#                    "PIL._imagingmorph" for Pillow's C extensions (build_pillow_objects, below).
# Setup.local (patches/python3/) adds pwd (+compat) and the _posixsubprocess stub.
# Not built (n/a below): no fork/exec (_posixshmem), no mmap, no tty (termios), no getrusage/rlimit
# (resource), _uuid (the deps/uuid stub has no uuid_generate_time: a random UUID would be reported as
# uuid1; uuid.py's pure fallback is correct), no libffi (_ctypes is a setup.py module, never run when
# cross compiling), no syslog/gdbm/nis/tk. _multiprocessing builds (named semaphores link; at runtime
# they need processes anyway).
PY_VER=3.11
PY_SHA512=11e06f2ffe1f66888cb5b4e9f607de815294d6863a77eda6ec6d7c724ef158df9f51881f4a956d4a6fa973c2fb6fd031d495e3496e9b0bb53793fb1cc8434c63
PY_HOST="$KODI/hosttools/python3"
PY_NA_MODULES=(_posixsubprocess _posixshmem mmap termios resource _uuid
  _xxsubinterpreters spwd syslog nis ossaudiodev _crypt _gdbm _tkinter _scproxy
  _testcapi _testinternalcapi _testbuffer _testclinic _testimportmultiple _testmultiphase _xxtestfuzz
  _ctypes_test xxlimited xxlimited_35)
# Cache answers for a cross build; functions declared but not linkable (configure only compile-tests
# them; the python link test found fchdir/fdatasync/setgroups/chroot); plus functions newlib/libsysbase
# link but cannot do (ENOSYS stubs:
# fork/exec/wait/kill/link/system) or that would touch console state (clock_settime): left out so
# Python does not expose os.fork() etc. as if they worked (darwin_embedded's recipe does the same).
PY_CACHE=(ac_cv_file__dev_ptmx=no ac_cv_file__dev_ptc=no ac_cv_buggy_getaddrinfo=no
  ac_cv_pthread_is_default=yes ac_cv_func_dlopen=no ac_cv_lib_dl_dlopen=no
  ac_cv_func_fork=no ac_cv_func_fork1=no ac_cv_func_vfork=no ac_cv_func_execv=no ac_cv_func_fexecve=no
  ac_cv_func_system=no ac_cv_func_posix_spawn=no ac_cv_func_posix_spawnp=no
  ac_cv_func_forkpty=no ac_cv_lib_util_forkpty=no ac_cv_func_openpty=no ac_cv_lib_util_openpty=no
  ac_cv_func_wait=no ac_cv_func_wait3=no ac_cv_func_wait4=no ac_cv_func_waitpid=no ac_cv_func_waitid=no
  ac_cv_func_kill=no ac_cv_func_killpg=no ac_cv_func_link=no ac_cv_func_linkat=no
  ac_cv_func_getentropy=no ac_cv_func_getrandom=no ac_cv_func_clock_settime=no ac_cv_func_settimeofday=no
  ac_cv_func_sched_setaffinity=no ac_cv_func_setpriority=no ac_cv_func_nice=no
  ac_cv_header_sys_resource_h=no ac_cv_working_tzset=yes
  ac_cv_func_getpwuid=yes ac_cv_func_getpwent=yes ac_cv_func_getgrgid=yes
  ac_cv_func_fchdir=no ac_cv_func_fdatasync=no ac_cv_func_setgroups=no ac_cv_func_chroot=no)
build_python3_host() {
  if [ -x "$PY_HOST/bin/python$PY_VER" ] && [ "$("$PY_HOST/bin/python$PY_VER" -c 'import sys; print(sys.version.split()[0])')" = "$(field python3 1)" ]; then
    return 0
  fi
  local tb d; tb="$(fetch python3)"; d="$BUILD/python3-host"
  rm -rf "$d"; mkdir -p "$d"; tar --no-same-owner -xf "$tb" -C "$d" --strip-components=1
  ( cd "$d"
    env -u CC -u CXX -u AR -u RANLIB -u NM -u STRIP -u LD -u CPPFLAGS -u CFLAGS -u CXXFLAGS -u LDFLAGS \
        -u LIBS -u PKG_CONFIG_LIBDIR -u ARCH bash -c "./configure --prefix='$PY_HOST' --without-ensurepip \
        --disable-test-modules >/dev/null && make -j$JOBS >/dev/null && make install >/dev/null" )
  "$PY_HOST/bin/python$PY_VER" -c 'import sys, zlib, ssl, sqlite3; print("host", sys.version)'
}
# Pillow 8.4.0 for script.module.pil: Kodi 21's tools/depends/target/pythonmodule-pil version (the PyPI sdist,
# its sha256 from PyPI's JSON). Pillow's setup.py is not run (it builds .so files and probes the build machine):
# its C extensions are compiled here from the same source lists (_IMAGING/_LIB_IMAGING, read from setup.py
# without executing it) with setup.py's defines for the libraries found (HAVE_LIBJPEG, HAVE_LIBZ,
# PILLOW_VERSION) and Kodi's recipe's --disable-* set (no jpeg2000/tiff/webp/imagequant/xcb/lcms; no raqm,
# so ImageFont uses the basic layout). Built: PIL._imaging (zlib + libjpeg), PIL._imagingft (FreeType),
# PIL._imagingmath, PIL._imagingmorph. Not built: _webp (Kodi's recipe passes --disable-webp; the
# portlibs' libwebp is 1.0.2 from 2019), _imagingcms (no lcms2 in
# the portlibs), _imagingtk (no Tk). The objects (pil_*.o) are added to libpython3.11.a, whose inittab
# (patch 0013) names them; the libraries are the devkit's portlibs (/opt/devkitpro/portlibs/switch):
# libjpeg-turbo 2.1.2, zlib 1.3.1, FreeType 2.13.3 (which needs its png16/harfbuzz/bz2). Kodi links them
# through xbmc/cmake/modules/FindPython.cmake (Kodi itself already links zlib/FreeType).
# The pure-Python package goes to prefix/share/script.module.pil/lib/PIL (package.sh copies it).
PIL_PORTLIBS="$DEVKITPRO/portlibs/switch"
PIL_LINK_LIBS="-ljpeg -lfreetype -lharfbuzz -lfreetype -lpng16 -lbz2 -lz"
build_pillow_objects() { # build_pillow_objects <python3 build dir> -> $BUILD/pillow/obj/pil_*.o + libpil.a
  local py="$1" pil o s p n f m=() pids=() srcs=() common=() core=()
  pil="$(unpack pillow)"; o="$pil/obj"; mkdir -p "$o"
  # patches/pillow/ (each on __SWITCH__; Pillow's modules assume separate .so files):
  #  0001 Convert.c   no global round(): it would replace libm's for all of kodi.bin (libpython3.11.a
  #                   precedes libm on the link line);
  #  0003 features.py pilinfo() prints "built-in" for Image.core (no __file__ on a built-in module);
  #  0004 Quant.c, Access.c  internal failures return an error instead of exit(1) (which would end Kodi).
  for p in "$PATCHES"/pillow/*.patch; do patch -d "$pil" -p1 -i "$p"; done
  mapfile -t srcs < <("$PY_HOST/bin/python$PY_VER" -I - "$pil/setup.py" <<'PY'
import ast, sys
tree = ast.parse(open(sys.argv[1], encoding="utf-8").read())
lists = {n.targets[0].id: ast.literal_eval(n.value) for n in tree.body
         if isinstance(n, ast.Assign) and len(n.targets) == 1 and isinstance(n.targets[0], ast.Name)
         and n.targets[0].id in ("_IMAGING", "_LIB_IMAGING")}
print("src/_imaging.c")
for s in lists["_IMAGING"]:
    print("src/%s.c" % s)
for s in lists["_LIB_IMAGING"]:
    print("src/libImaging/%s.c" % s)
PY
)
  # Guard against a silent parse failure (process substitution hides the exit code).
  for f in src/_imaging.c src/decode.c src/libImaging/JpegDecode.c src/libImaging/ZipEncode.c src/libImaging/codec_fd.c; do
    printf '%s\n' "${srcs[@]}" | grep -qx "$f" || { echo "setup.py source list lacks $f" >&2; return 1; }
  done
  srcs+=(src/_imagingft.c src/_imagingmath.c src/_imagingmorph.c)
  # As a setuptools extension build: Python's own -fwrapv -DNDEBUG, no Py_BUILD_CORE. gnu17: GCC 15
  # defaults to C23, which Pillow 8.4 (2021) predates.
  # shellcheck disable=SC2206
  common=($CPPFLAGS $CFLAGS -fno-omit-frame-pointer -fwrapv -DNDEBUG -std=gnu17
    -I"$py/Include" -I"$py" -I"$pil/src/libImaging" -I"$PIL_PORTLIBS/include")
  core=(-DHAVE_LIBJPEG -DHAVE_LIBZ "-DPILLOW_VERSION=\"$(field pillow 1)\"")
  n=0
  for s in "${srcs[@]}"; do
    case "$s" in
      src/_imagingft.c) m=(-I"$PIL_PORTLIBS/include/freetype2") ;;
      src/_imagingmath.c|src/_imagingmorph.c) m=() ;;
      *) m=("${core[@]}") ;;
    esac
    f="$o/pil_$(basename "$s" .c).o"
    [ -e "$f" ] && { echo "duplicate object name $f" >&2; return 1; }
    "$CC" "${common[@]}" "${m[@]}" -c "$pil/$s" -o "$f" & pids+=($!)
    n=$((n + 1))
    if [ ${#pids[@]} -ge "$JOBS" ]; then wait "${pids[0]}"; pids=("${pids[@]:1}"); fi
  done
  for p in "${pids[@]}"; do wait "$p"; done
  [ "$(ls "$o"/pil_*.o | wc -l)" -eq "$n" ] || { echo "pillow: $n sources, fewer objects" >&2; return 1; }
  rm -f "$o/libpil.a"; "$AR" rcs "$o/libpil.a" "$o"/pil_*.o
  echo "pillow: $n objects"
}
# pil_symbol_guard <libpython3.11.a without Pillow>: no global that Pillow defines may also be defined by
# CPython, newlib/libounce or a portlib (nor twice inside Pillow). In the static link the first archive
# wins silently, and libpython3.11.a comes first: other callers would bind to Pillow's copy (round(),
# before patch pillow/0001).
pil_symbol_guard() {
  local a dup
  dup="$("$NM" -g --defined-only "$BUILD/pillow/obj/libpil.a" 2>/dev/null | awk 'NF==3 && $2 ~ /[TDBRCVG]/ {print $3}' | sort | uniq -d)"
  [ -z "$dup" ] || { echo "Pillow globals defined twice inside Pillow: $dup" >&2; return 1; }
  dup="$( { "$NM" -g --defined-only "$BUILD/pillow/obj/libpil.a" 2>/dev/null | awk 'NF==3 && $2 ~ /[TDBRCVWG]/ {print "P", $3}'
            for a in "$1" "$DEVKITPRO"/devkitA64/aarch64-none-elf/lib/pic/libc.a \
                     "$DEVKITPRO"/devkitA64/aarch64-none-elf/lib/pic/libm.a "$DEVKITPRO"/libounce/lib/*.a \
                     "$PIL_PORTLIBS"/lib/*.a "$PREFIX"/lib/lib*.a; do
              case "$a" in */libpython$PY_VER.a) [ "$a" = "$1" ] || continue ;; esac
              "$NM" -g --defined-only "$a" 2>/dev/null | awk 'NF==3 && $2 ~ /[TDBRCVWG]/ {print "O", $3}'
            done; } | awk '$1=="P"{p[$2]=1; next} ($2 in p) && !s[$2]++ {print $2}')"
  [ -z "$dup" ] || { echo "Pillow globals also defined elsewhere: $dup" >&2; return 1; }
}
build_python3() {
  local tb d p m na=(); tb="$(fetch python3)"
  echo "$PY_SHA512  $tb" | sha512sum -c
  build_python3_host
  d="$(unpack python3)"; cd "$d"
  for p in "$PATCHES"/python3/*.patch; do patch -p1 -i "$p"; done
  install -m644 "$PATCHES/python3/Setup.local" Modules/Setup.local
  for m in "${PY_NA_MODULES[@]}"; do na+=("py_cv_module_$m=n/a"); done
  # Frame records in every function, as Kodi's own code (CrashHandler.cpp walks x29).
  CFLAGS="$CFLAGS -fno-omit-frame-pointer" \
  ./configure --host="$HOST" --build=x86_64-linux-gnu --prefix="$PREFIX" \
    --disable-shared --without-ensurepip --disable-test-modules --disable-ipv6 \
    --with-build-python="$PY_HOST/bin/python$PY_VER" --with-openssl="$PREFIX" --with-openssl-rpath=no \
    --without-system-expat --without-system-libmpdec --without-readline --with-pymalloc \
    MODULE_BUILDTYPE=static "${PY_CACHE[@]}" "${na[@]}"
  make -j"$JOBS" libpython$PY_VER.a
  # Pillow's C extensions into the same archive: patch 0013's inittab (config.o) names them, and Kodi
  # links only the archive (an archive's members resolve each other in any order).
  build_pillow_objects "$d"
  pil_symbol_guard "$d/libpython$PY_VER.a"
  "$AR" rs libpython$PY_VER.a "$BUILD"/pillow/obj/pil_*.o
  # Link test: the stock interpreter must link against newlib + libounce alone (not shipped). Kodi
  # links the same archive, so a symbol missing here is missing in kodi.bin too.
  # (LDFLAGS= : configure already copied them into CONFIGURE_LDFLAGS; twice repeats -specs/switch.ld.)
  # 3.11 links python from $(LIBRARY_OBJS), not from the archive: Pillow's objects (libpil.a) and its
  # libraries (PIL_LINK_LIBS, the portlibs Kodi links) go in through SYSLIBS.
  make -j"$JOBS" LDFLAGS= SYSLIBS="$BUILD/pillow/obj/libpil.a $PIL_LINK_LIBS \$(LIBM) \$(LIBC)" python
  make pybuilddir.txt
  make inclinstall
  install -m644 libpython$PY_VER.a "$PREFIX/lib/"
  # The bundled libmpdec (_decimal) and expat (pyexpat, _elementtree) are separate archives.
  install -m644 Modules/_decimal/libmpdec/libmpdec.a "$PREFIX/lib/libpython$PY_VER-mpdec.a"
  install -m644 Modules/expat/libexpat.a "$PREFIX/lib/libpython$PY_VER-expat.a"
  # Pure-Python stdlib for the SD card (package.sh copies it to /switch/kodi/system/python).
  local lib="$PREFIX/lib/python$PY_VER" bld
  rm -rf "$lib"; mkdir -p "$lib/lib-dynload" "$lib/site-packages"
  ( cd Lib && tar --exclude='__pycache__' --exclude='./test' --exclude='./idlelib' --exclude='./tkinter' \
      --exclude='./turtledemo' --exclude='./ensurepip' --exclude='./turtle.py' --exclude='./lib2to3/tests' \
      --exclude='./distutils/tests' --exclude='./unittest/test' --exclude='./ctypes/test' \
      --exclude='./sqlite3/test' --exclude='./site-packages' -cf - . ) | tar -C "$lib" -xf -
  bld="$(cat pybuilddir.txt)"
  install -m644 "$bld"/_sysconfigdata_*.py "$lib/"
  # .pyc next to the sources, unchecked-hash: SD copies change mtimes, so timestamp pycs would all be
  # recompiled (and rewritten) on the console. Same 3.11.7 magic as the target. Also a syntax check
  # of every shipped .py with the host interpreter.
  "$PY_HOST/bin/python$PY_VER" -m compileall -q -j0 --invalidation-mode unchecked-hash \
    -s "$lib" -p "/switch/kodi/system/python/lib/python$PY_VER" "$lib"
  # Pillow's pure-Python package for script.module.pil (package.sh puts it in the add-on's lib/, the
  # library="lib" of its addon.xml), with unchecked-hash .pyc like the stdlib.
  local pilpy="$PREFIX/share/script.module.pil/lib"
  rm -rf "$PREFIX/share/script.module.pil"; mkdir -p "$pilpy"
  ( cd "$BUILD/pillow/src" && tar --exclude='__pycache__' -cf - PIL ) | tar -C "$pilpy" -xf -
  "$PY_HOST/bin/python$PY_VER" -m compileall -q -j0 --invalidation-mode unchecked-hash \
    -s "$pilpy" -p "/switch/kodi/addons/script.module.pil/lib" "$pilpy"
  [ -f "$pilpy/PIL/__pycache__/Image.cpython-311.pyc" ]
  "$NM" --defined-only "$PREFIX/lib/libpython$PY_VER.a" 2>/dev/null | grep -E ' T PyInit_' | awk '{print $3}' | sort >"$BUILD/python3-inittab.txt"
  cat "$BUILD/python3-inittab.txt"
  has_sym libpython$PY_VER.a Py_InitializeEx
  has_sym libpython$PY_VER.a PyInit__ssl
  has_sym libpython$PY_VER.a PyInit__socket
  has_sym libpython$PY_VER.a PyInit__sqlite3
  has_sym libpython$PY_VER.a PyInit__imaging
  has_sym libpython$PY_VER.a PyInit__imagingft
  has_sym libpython$PY_VER.a PyInit__imagingmath
  has_sym libpython$PY_VER.a PyInit__imagingmorph
  has_sym libpython$PY_VER.a ImagingJpegDecode
}

# ---------------------------------------------------------------- libnfs
# Kodi's pinned 5.0.2 (tools/depends/target/libnfs; its sha512 matches), the same cmake options as
# Kodi's recipe, with libounce's Switch 2 toolchain. patches/libnfs/0001-switch2-no-ipv6-nobody.patch
# (each hunk on __SWITCH__): AUTH_UNIX uid/gid 65534 (newlib has no getuid/getgid, as on Windows);
# lib/switch2_compat.h, force-included: htonl/ntohl (<arpa/inet.h>), struct utimbuf, SOL_TCP,
# major()/minor();
# no AF_INET6 sockets (libounce's sockaddr_in6 has no sin6_len, so its family overlays the BSD
# sa_len byte; CPython here is --disable-ipv6 for the same reason).
build_libnfs() {
  local d; d="$(unpack libnfs)"; cd "$d"
  patch -p1 -i "$PATCHES/libnfs/0001-switch2-no-ipv6-nobody.patch"
  # The toolchain file brings the arch flags, -specs and libounce itself: not again from the env.
  env -u LDFLAGS -u LIBS -u CFLAGS -u CPPFLAGS -u CC -u CXX -u AR -u RANLIB -u NM -u STRIP -u LD \
  cmake -S . -B build -DCMAKE_TOOLCHAIN_FILE="$DEVKITPRO/libounce/cmake/Switch2.cmake" \
    -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$PREFIX" -DCMAKE_INSTALL_LIBDIR=lib \
    -DBUILD_SHARED_LIBS=OFF -DENABLE_TESTS=OFF -DENABLE_DOCUMENTATION=OFF -DENABLE_UTILS=OFF \
    -DENABLE_EXAMPLES=OFF -DHAVE_UTIME_H=1 -DCMAKE_C_FLAGS="-D__SWITCH__ -include $d/lib/switch2_compat.h"
  cmake --build build -j"$JOBS"
  cmake --install build
  has_sym libnfs.a nfs_init_context
  has_sym libnfs.a nfs_set_timeout
  has_sym libnfs.a mount_getexports_timeout
}
# ---------------------------------------------------------------- libmicrohttpd
# Kodi's pinned 0.9.77 (tools/depends/target/libmicrohttpd). Kodi's recipe has --enable-https (via
# GnuTLS, which this port does not build): HTTPS is off, plain HTTP for the web interface/JSON-RPC.
# Threads: pthreads; polling: select()/poll() of libounce's BSD sockets (no epoll on newlib).
build_libmicrohttpd() {
  local d; d="$(unpack libmicrohttpd)"; cd "$d"
  ./configure --host="$HOST" --prefix="$PREFIX" --disable-shared --enable-static --disable-doc \
    --disable-examples --disable-curl --disable-https --disable-epoll --without-gnutls \
    --disable-tools --enable-itc=socketpair "${MHD_CACHE[@]}"
  make -j"$JOBS" -C src/microhttpd
  make -C src/microhttpd install
  make -C src/include install
  make install-pkgconfigDATA 2>/dev/null || install -m644 libmicrohttpd.pc "$PREFIX/lib/pkgconfig/"
  rm -f "$PREFIX"/lib/libmicrohttpd.la
  has_sym libmicrohttpd.a MHD_start_daemon
}
MHD_CACHE=()

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
