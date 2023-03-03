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
ARCHIVE_SHA256SUM=$DESTDIR.sha256.txt

echo "building from scratch in $BUILDDIR"
rm -rf $BUILDDIR
./build.sh -opt -out=$BUILDDIR

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

echo "creating $DESTDIR.tar.xz"
_create_tar_xz_from_dir $DESTDIR $DESTDIR.tar.xz

echo "creating $DESTDIR.sha256.txt"
( cd $(dirname $DESTDIR) &&
  sha256sum $(basename $DESTDIR.tar.xz) > $(basename $DESTDIR.sha256.txt)
)
