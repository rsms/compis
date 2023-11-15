#!/usr/bin/env bash
set -euo pipefail
source "$(dirname "$0")/lib.sh"
_pushd "$PROJECT"

FORCE=false
TEST=true
NO_CODESIGN=false
CLEAN=true
CREATE_TAR=true
TARGET=
# note: build.sh validates the target string for us

while [[ $# -gt 0 ]]; do case "$1" in
  --force)       FORCE=true; shift ;;
  --no-test)     TEST=false; shift ;;
  --no-codesign) NO_CODESIGN=true; shift ;;
  --no-clean)    CLEAN=false; shift ;;
  --no-tar)      CREATE_TAR=false; shift ;;
  -h|-help|--help) cat << _END
Usage: $0 [options] <target>
Options:
  --force        Create distribution even if some preconditions are not met
  --no-test      Skip tests
  --no-codesign  Don't codesign (macos only; ignored for other targets)
  --no-clean     Don't build from scratch (only use this for debugging!)
  --no-tar       Don't create tar archive of the result
  -h, --help     Show help on stdout and exit
<target> is one of:
  aarch64-linux
  aarch64-macos
  x86_64-linux
  x86_64-macos
_END
    exit ;;
  -*) _err "Unknown option: $1" ;;
  *) [ -z "$TARGET" ] || _err "extranous argument: $1"; TARGET=$1; shift ;;
esac; done

[ -n "$TARGET" ] || _err "missing <target>"

CO_VERSION=$(cat "$PROJECT/version.txt")
BUILDDIR=out/dist
DESTDIR=out/dist/compis-$CO_VERSION-$TARGET
ARCHIVE=out/compis-$CO_VERSION-$TARGET.tar.xz

if ! $FORCE && [ -d .git ] && [ -n "$(git status -s | grep -v '?')" ]; then
  echo "uncommitted git changes:" >&2
  git status -s | grep -v '?' >&2
  exit 1
fi

CODESIGN=false
if [[ "$TARGET" == *-macos ]] && ! $NO_CODESIGN; then
  CODESIGN=true
  if [ -z "${CODESIGN_ID:-}" ]; then
    if command -v security >/dev/null; then
      #echo "CODESIGN_ID: looking up with 'security find-identity -p codesigning'"
      CODESIGN_ID=$(security find-identity -p codesigning |
                    grep 'Developer ID Application:' | head -n1 | awk '{print $2}')
    fi
    if [ -z "${CODESIGN_ID:-}" ]; then
      echo "warning: macos code signing disabled (no signing identity found)" >&2
      CODESIGN=false
    fi
  fi
fi

# generate sysdir
if $CLEAN || _need_regenerate_sysinc_dir; then
  _regenerate_sysinc_dir
fi

# build compis
if $CLEAN; then
  echo "building from scratch in $BUILDDIR"
  rm -rf $BUILDDIR
else
  echo "building incrementally in $BUILDDIR"
fi
./build.sh -target=$TARGET -DCO_DISTRIBUTION=1 -opt -out=$BUILDDIR

# create & copy files to DESTDIR
echo "creating $DESTDIR"
rm -rf $DESTDIR
mkdir -vp $DESTDIR/lib $DESTDIR
install -vm755 $BUILDDIR/co $DESTDIR/compis
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

# create tool symlinks, e.g. cc -> compis
_create_tool_symlinks $DESTDIR/compis

# deduplicate files using hardlinks.
# This allows the tar to encode hardlinks, which doesn't affect compressed tar
# file size much, but does allow creation of hardlinks when the tar is unarchived.
# Comparison:
#   tar creation without fdupes:  23.4 MiB / 176.5 MiB = 0.133 (3.6 MiB/s)
#   tar creation with fdupes:     23.2 MiB / 167.8 MiB = 0.138 (3.5 MiB/s)
fdupes="$DEPS_DIR/fdupes/fdupes"
if [ ! -x "$fdupes" ]; then
  git clone https://github.com/tobiasschulz/fdupes.git "$DEPS_DIR/fdupes"
  make -C "$DEPS_DIR/fdupes" CC="$PWD/$DESTDIR/cc" -j$(nproc)
fi
echo "deduplicating identical files using hardlinks"
"$fdupes" --recurse --linkhard --noempty $DESTDIR >/dev/null

if $TEST; then
  if [ "$TARGET" != "$HOST_ARCH-$HOST_SYS" ]; then
    echo "warning: disabling tests since target is different from host" >&2
    TEST=false
  else
    echo "running test/test-build-sysroot-race.sh"
    COEXE=$DESTDIR/compis ./test/test-build-sysroot-race.sh
    echo "running test/test.sh"
    ./test/test.sh --coexe=$DESTDIR/compis
  fi
fi

if $CODESIGN; then
  echo "codesign using identity $CODESIGN_ID"
  codesign -s "$CODESIGN_ID" -f -i me.rsms.compis $DESTDIR/compis
fi

if $CREATE_TAR; then
  echo "creating $ARCHIVE"
  _create_tar_xz_from_dir $DESTDIR $ARCHIVE
fi
