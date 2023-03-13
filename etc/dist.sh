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

if [ -d .git ] && [ -n "$(git status -s | grep -v '?')" ]; then
  echo "uncommitted git changes:" >&2
  git status -s | grep -v '?' >&2
  exit 1
fi

if [ "$SYS" = macos -a -z "${CODESIGN_ID:-}" ]; then
  if command -v security >/dev/null; then
    echo "CODESIGN_ID: looking up with 'security find-identity -p codesigning'"
    CODESIGN_ID=$(security find-identity -p codesigning |
                  grep 'Developer ID Application:' | head -n1 | awk '{print $2}')
  fi
  if [ -z "${CODESIGN_ID:-}" ]; then
    echo "warning: macos code signing disabled (no signing identity found)" >&2
  fi
fi

_regenerate_sysinc_dir

echo "building from scratch in $BUILDDIR"
rm -rf $BUILDDIR
./build.sh -DCO_DISTRIBUTION=1 -opt -out=$BUILDDIR

echo "creating $DESTDIR"
rm -rf $DESTDIR
mkdir -p $DESTDIR/lib $DESTDIR
mv $BUILDDIR/co $DESTDIR/compis
for f in lib/*; do
  case $f in
    lib/|.*|*-src|README*) continue ;;
    # disallow "build", in case a user builds inside coroot (e.g. to test compis)
    */build) _err "$f conflicts with file or directory created at runtime"
  esac
  _copy $f $DESTDIR/$(basename $f)
done
find $DESTDIR -type f -iname 'README*' -delete
find $DESTDIR -empty -type d -delete

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

# codesign macos exe
if [ "$SYS" = macos -a -n "${CODESIGN_ID:-}" ]; then
  echo "codesign using identity $CODESIGN_ID"
  codesign -s "$CODESIGN_ID" -f -i me.rsms.compis $DESTDIR/compis
fi

echo "creating $ARCHIVE"
_create_tar_xz_from_dir $DESTDIR $ARCHIVE
