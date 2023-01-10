#!/bin/bash
set -e
INITIAL_PWD=$PWD
cd "$(dirname "$0")"
ETC_LLVM_DIR=$PWD
. ../lib.sh
cd ../..  # to PROJECT

LLVM_RELEASE=14.0.0
LLVM_RELEASE_TAR_SHA256=87b1a068b370df5b79a892fdb2935922a8efb1fddec4cc506e30fe57b6a1d9c4
LLVM_SRCDIR=$DEPS_DIR/llvm-src-$LLVM_RELEASE
LLVM_SRCDIR_CHANGED=false
LLVM_BUILD_DIR=$LLVM_SRCDIR/build2
LLVM_DESTDIR=$DEPS_DIR/llvm2

if [ ! -d "$LLVM_SRCDIR" ]; then
  _download \
    https://github.com/llvm/llvm-project/archive/llvmorg-${LLVM_RELEASE}.tar.gz \
    $LLVM_RELEASE_TAR_SHA256
  _extract_tar \
    "$(_downloaded_file llvmorg-${LLVM_RELEASE}.tar.gz)" \
    "$LLVM_SRCDIR"
  LLVM_SRCDIR_CHANGED=true
fi

# rm -rf "$LLVM_BUILD_DIR"
mkdir -p "$LLVM_BUILD_DIR"
_pushd "$LLVM_BUILD_DIR"

cp "$ETC_LLVM_DIR/stage1.cmake" stage1.cmake
cp "$ETC_LLVM_DIR/stage2.cmake" stage2.cmake

cmake -G Ninja \
  -C stage1.cmake \
  -DCMAKE_INSTALL_PREFIX="$LLVM_DESTDIR" \
  "$LLVM_SRCDIR/llvm"

# build libc++ with host compiler
ninja cxx

export LD_LIBRARY_PATH=$PWD/lib
# Second, build the rest against it. This includes a new version of libc++, this
# time built with stage-1 clang, which replaces the one built with the host
# compiler.
ninja stage2-install-distribution
