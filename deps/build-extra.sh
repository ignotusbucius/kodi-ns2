#!/usr/bin/env bash
# Extra static deps for the Kodi Switch 2 port that Kodi's own internal recipes cannot cross-build.
# Run inside kodi-switch2-devkit after `. /opt/devkitpro/switch2vars.sh`. Sources are fetched on the host (fetch step).
set -euo pipefail
K=/k; P=$K/prefix; S=$K/deps/src-extra
HOST=aarch64-none-elf
export PKG_CONFIG_PATH=$P/lib/pkgconfig
export CPPFLAGS="$CPPFLAGS -I$P/include"
export LDFLAGS="$LDFLAGS -L$P/lib -specs=/opt/devkitpro/libounce/switch.specs"

if [ ! -f $P/.stamp-fstrcmp ]; then
  # fstrcmp's libtool is the host's, so compile its handful of C files straight into an archive.
  cd $S && rm -rf fstrcmp-0.7.D001 && tar xzf fstrcmp-0.7.D001.tar.gz && cd fstrcmp-0.7.D001
  CC=$HOST-gcc ./configure --host=$HOST --prefix=$P >/dev/null
  rm -f lib/*.o lib/ac/*.o
  for c in lib/*.c lib/ac/*.c; do $HOST-gcc $CFLAGS $CPPFLAGS -I. -c "$c" -o "${c%.c}.o"; done
  $HOST-ar rcs $P/lib/libfstrcmp.a lib/*.o lib/ac/*.o
  install -m644 lib/fstrcmp.h $P/include/fstrcmp.h
  cat > $P/lib/pkgconfig/fstrcmp.pc <<PC
prefix=$P
libdir=\${prefix}/lib
includedir=\${prefix}/include
Name: fstrcmp
Description: fuzzy string compare
Version: 0.7.D001
Libs: -L\${libdir} -lfstrcmp
Cflags: -I\${includedir}
PC
  touch $P/.stamp-fstrcmp
fi
ls -l $P/lib/libfstrcmp.a

if [ ! -f $P/.stamp-uuid ]; then
  # crossguid wants libuuid; this stand-in generates random (v4) UUIDs with randomGet().
  mkdir -p $P/include/uuid
  $HOST-gcc $CFLAGS $CPPFLAGS -c $K/deps/uuid/uuid.c -o /tmp/uuid.o
  $HOST-ar rcs $P/lib/libuuid.a /tmp/uuid.o
  install -m644 $K/deps/uuid/uuid.h $P/include/uuid/uuid.h
  touch $P/.stamp-uuid
fi
ls -l $P/lib/libuuid.a
