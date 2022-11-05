#!/bin/bash
#
# tests c0 cc
# usage: test-cc.sh [<c0exe>]
#
set -e
cd "$(dirname "$0")"/../..  # to project root

C0=${C0:-$1} ; C0=${C0:-build/debug/c0}
OS=$(uname -s)
LD=$(dirname "$(realpath "$C0")")/ld
CFLAGS=( $CFLAGS )
LDFLAGS=( $LDFLAGS )
CFLAGS_LD=( -fuse-ld="$LD" )

case "$OS" in
  Darwin)
    LDFLAGS+=(
      -arch $(uname -m) \
      -platform_version macos 10.15 10.15 \
      -lSystem \
    )
    CFLAGS_LD+=(
      -Wl,-platform_version,macos,10.15,10.15 \
    )
    ;;
esac

SRC=etc/test-cc/test.c
OBJ=build/test-cc.o
EXE=build/test-cc
mkdir -p build

# we need a symlink to c0 called "ld" for "compile and link in one step"
ln -sf $(basename "$C0") $LD

set -x

# compile object and then link it into an executable
# Note: "PATH=" to ensure c0 is not using any system programs like ld
PATH= $C0 cc "${CFLAGS[@]}" -c $SRC -o $OBJ
PATH= $C0 ld "${LDFLAGS[@]}"   $OBJ -o $EXE
$EXE

# compile and link in one step
PATH= $C0 cc "${CFLAGS[@]}" "${CFLAGS_LD[@]}" $SRC -o $EXE
$EXE

set +x

echo "dylibs used:"
_list_dylibs_used() {
  case "$OS" in
    Darwin) deps/llvm/bin/llvm-objdump --macho --dylibs-used "$@" ;;
    *)      echo "TODO _list_dylibs_used for $OS"; exit 1 ;;
  esac
}
_list_dylibs_used $C0
_list_dylibs_used $EXE
