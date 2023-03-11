#!/usr/bin/env bash
set -euo pipefail
source "$(dirname "$0")/lib.sh"
_pushd "$PROJECT"

ARCH=$(uname -m) ; ARCH=${ARCH/arm64/aarch64}
SYS=$(uname -s) ; case "$SYS" in
  Darwin) SYS=macos ;;
  *)      SYS=$(awk '{print tolower($0)}' <<< "$SYS") ;;
esac

CO_VERSION=$(cat "$PROJECT/version.txt")
BUILDDIR=out/dist
DESTDIR=out/dist/compis-$CO_VERSION-$ARCH-$SYS
ARCHIVE=out/compis-$CO_VERSION-$ARCH-$SYS.tar.xz

_regenerate_sysinc_dir

echo "building from scratch in $BUILDDIR"
rm -rf $BUILDDIR
./build.sh -DCO_DISTRIBUTION=1 -opt -out=$BUILDDIR

echo "creating $DESTDIR"
rm -rf $DESTDIR
mkdir -p $DESTDIR/lib
mv $BUILDDIR/co $DESTDIR/compis
for f in lib/*; do
  case $f in
    .*|*-src) continue ;;
    # disallow "build", in case a user builds inside coroot (e.g. to test compis)
    */build) _err "$f conflicts with file or directory created at runtime"
  esac
  _copy $f $DESTDIR/$(basename $f)
done

# create symlinks
for name in \
  cc \
  ar \
  as \
  ranlib \
  ld.lld \
  ld64.lld \
  wasm-ld \
  lld-link \
;do
  _symlink "$DESTDIR/$name" compis
done

echo "creating $ARCHIVE"
_create_tar_xz_from_dir $DESTDIR $ARCHIVE
