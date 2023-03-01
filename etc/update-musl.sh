#!/usr/bin/env bash
set -euo pipefail
source "$(dirname "$0")/lib.sh"

SUPPORTED_ARCHS=( aarch64 arm i386 riscv64 x86_64 )

MUSL_VERSION=1.2.3
MUSL_SHA256=7d5b0b6062521e4627e099e4c9dc8248d32a30285e959b7eecaa780cf8cfd4a4
MUSL_SRCDIR="$DEPS_DIR/musl-$MUSL_VERSION"
SOURCE_DESTDIR="$PROJECT/lib/musl"
HEADERS_DESTDIR="$PROJECT/lib/musl-headers"

[ ! -f "$MUSL_SRCDIR/Makefile" ] &&
  _download \
    https://musl.libc.org/releases/musl-$MUSL_VERSION.tar.gz \
    "$DOWNLOAD_DIR/musl-$MUSL_VERSION.tar.gz" \
    "$MUSL_SHA256" &&
  _extract_tar \
    "$DOWNLOAD_DIR/musl-$MUSL_VERSION.tar.gz" \
    "$MUSL_SRCDIR"

_pushd "$MUSL_SRCDIR"

# ————————————————————————————————————————————————————————————————————————————————————
# Copy sources.
# See musl Makefile

rm -rf "$SOURCE_DESTDIR"
mkdir -p "$SOURCE_DESTDIR/arch"

for f in \
  $(find src -type f -name '*.h') \
  compat/time32/*.c \
  crt/*.c \
  ldso/*.c \
  src/*/*.c \
  src/malloc/mallocng/*.c \
;do
  [ -f "$f" ] || continue
  mkdir -p $(dirname "$SOURCE_DESTDIR/$f")
  cp $f "$SOURCE_DESTDIR/$f"
done &

for arch in "${SUPPORTED_ARCHS[@]}"; do
  for f in \
    crt/$arch/*.[csS] \
    ldso/$arch/*.[csS] \
    src/*/$arch/*.[csS] \
    src/malloc/mallocng/$arch/*.[csS] \
  ;do
    [ -f "$f" ] || continue
    mkdir -p $(dirname "$SOURCE_DESTDIR/$f")
    cp $f "$SOURCE_DESTDIR/$f"
  done &
  # headers
  [ -d "arch/$arch" ] &&
    _copy "arch/$arch" "$SOURCE_DESTDIR/arch/$arch"
done
_copy "arch/generic" "$SOURCE_DESTDIR/arch/generic" &
( sleep 0.5 ; echo "(Waiting for copy jobs to finish...)" ) &
wait

# copy license statement
_copy COPYRIGHT "$SOURCE_DESTDIR"

# create version.h, needed by version.c (normally created by musl's makefile)
echo "generate $(_relpath "$SOURCE_DESTDIR/src/internal/version.h")"
echo "#define VERSION \"$MUSL_VERSION\"" > "$SOURCE_DESTDIR/src/internal/version.h"

# _popd
# rm -rf "$MUSL_SRCDIR"

# remove unused files and empty directories from SOURCE_DESTDIR
find "$SOURCE_DESTDIR" -type f \( -name '*.in' -o -name '*.mak' \) -delete
find "$SOURCE_DESTDIR" -empty -type d -delete

# ————————————————————————————————————————————————————————————————————————————————————
# install headers

for arch in "${SUPPORTED_ARCHS[@]}"; do
  ARCH_DESTDIR=$SOURCE_DESTDIR/include/$arch-linux

  echo "make install-headers $arch -> $(_relpath "$ARCH_DESTDIR")"

  rm -rf obj destdir
  make DESTDIR=destdir install-headers -j$(nproc) ARCH=$arch prefix= >/dev/null

  rm -rf "$ARCH_DESTDIR"
  mkdir -p "$(dirname "$ARCH_DESTDIR")"
  mv destdir/include "$ARCH_DESTDIR"
done

# ————————————————————————————————————————————————————————————————————————————————————
# remove duplicate headers

_pushd "$SOURCE_DESTDIR"

echo "Deduplicating headers..."

_remove_if_duplicate() { # <archdir> <srcfile>
  local archdir arch srcfile name targetdir shasum f
  archdir=$1 ; srcfile=$2
  name=${srcfile:$(( ${#archdir} + 1 ))}
  shasum=$(sha256sum "$srcfile" | cut -d' ' -f1)

  arch=${archdir#*/}
  targetdir=include/$arch-linux
  [ $arch = generic ] && targetdir=include/x86_64-linux

  f=$targetdir/$name
  if [ -f "$f" ] && [ "$(sha256sum "$f" | cut -d' ' -f1)" = "$shasum" ]; then
    #echo "rm duplicate $srcfile ($f)"
    rm $srcfile
  fi
}

for archdir in arch/*; do
  for srcfile in $(find $archdir -type f -name '*.h'); do
    _remove_if_duplicate "$archdir" "$srcfile"
  done
done

find . -empty -type d -delete

"$LLVMBOX"/bin/llvmbox-dedup-target-files include

# ————————————————————————————————————————————————————————————————————————————————————
# re-generate src/musl_info.h

exec $BASH "$PROJECT/etc/gen-musl_info.h.sh"
