#!/bin/bash
set -e
cd "$(dirname "$0")"
source etc/lib.sh
eval $(grep '^OUTDIR=' build.sh | head -n1)

if [[ " $@ " == *" -h "* ]] || [[ " $@ " == *" --help "* ]]; then
  cat <<END
Initializes dependencies
usage: $0 [<arg to etc/llvm/build-llvm.sh> ...]
END
  exit
fi

# echo "ARGV0          = $ARGV0"
# echo "SCRIPT_FILE    = $SCRIPT_FILE"
# echo "PROJECT        = $PROJECT"
# echo "HOST_SYS       = $HOST_SYS"
# echo "HOST_ARCH      = $HOST_ARCH"
# echo "DEPS_DIR       = $DEPS_DIR"
# echo "WORK_DIR       = $WORK_DIR"
# echo "WORK_BUILD_DIR = $WORK_BUILD_DIR"
# echo "DOWNLOAD_DIR   = $DOWNLOAD_DIR"
# echo "TMPFILES_LIST  = $TMPFILES_LIST"
# echo "OPT_QUIET      = $OPT_QUIET"
# echo "INITIAL_PWD    = $INITIAL_PWD"

_err() { echo -e "$0:" "$@" >&2 ; exit 1; }

# ---------- host utilities ----------
# check for programs and shell features needed

_hascmd() { command -v "$1" >/dev/null || return 1; }
_needcmd() {
  while [ $# -gt 0 ]; do
    if ! _hascmd "$1"; then
      _err "missing $1 -- please install or use a different shell"
    fi
    shift
  done
}
_needcmd \
  pushd popd basename head grep stat awk \
  tar git cmake ninja sha256sum
_hascmd curl || _hascmd wget || _err "curl nor wget found in PATH"

rm -f $DEPS_DIR/configured
mkdir -p $DEPS_DIR


echo "---------- llvm ----------"

# build LLVM
echo bash etc/llvm/build-llvm.sh "$@"
     bash etc/llvm/build-llvm.sh "$@"

export PATH=$DEPS_DIR/llvm/bin:$PATH
export CC=clang
export CXX=clang++


echo "---------- nim ----------"

_download_pushsrc \
  https://github.com/nim-lang/Nim/archive/refs/tags/v1.6.8.tar.gz \
  a12466ed07713818c5ed5d7a56c647d30075a3989e7ac2a6e7696b1e0796a281 \
  nim-v1.6.8.tar.gz

ls -l

_popsrc

# ---------- test compiler ----------
if ! [ -f $DEPS_DIR/cc-tested ]; then
  CC_TEST_DIR=$OUTDIR/cc-test
  rm -rf $CC_TEST_DIR
  mkdir -p $CC_TEST_DIR
  pushd $CC_TEST_DIR >/dev/null
  echo "Running tests in $CC_TEST_DIR"

  cat << _END_ > hello.c
#include <stdio.h>
int main(int argc, const char** argv) {
  printf("hello from %s\n", argv[0]);
  return 0;
}
_END_

  cat << _END_ > hello.cc
#include <iostream>
int main(int argc, const char** argv) {
  std::cout << "hello from " << argv[0] << "\n";
  return 0;
}
_END_

  printf "CC=";  command -v $CC
  printf "CXX="; command -v $CXX

  CC_TEST_STATIC=1
  if [ "$HOST_SYS" = "Darwin" ]; then
    # can't link libc statically on mac
    CC_TEST_STATIC=
  fi

  echo "Compile C and C++ test programs:"
  set -x
  $CC  -Wall -std=c17     -O2         -o hello_c_d  hello.c
  $CXX -Wall -std=gnu++17 -O2         -o hello_cc_d hello.cc
  if [ -n "$CC_TEST_STATIC" ]; then
    $CC  -Wall -std=c17     -O2 -static -o hello_c_s  hello.c
    $CXX -Wall -std=gnu++17 -O2 -static -o hello_cc_s hello.cc
  fi
  set +x
  echo "Compile test programs: OK"

  echo "Run test programs:"
  for f in hello_c_d hello_cc_d; do
    ./${f}
    strip -o stipped_$f $f
    ./stipped_$f
  done
  if [ -n "$CC_TEST_STATIC" ]; then
    for f in hello_c_s hello_cc_s; do
      ./${f}
      strip -o stipped_$f $f
      ./stipped_$f
    done
  fi
  echo "Run test programs: OK"

  echo "Verifying linking..."
  LD_VERIFICATION_TOOL=llvm-objdump
  if [ "$HOST_SYS" = "Darwin" ]; then
    _needcmd otool
    LD_VERIFICATION_TOOL=otool
    _has_dynamic_linking() { otool -L "$1" | grep -q .dylib; }
  else
    _has_dynamic_linking() { llvm-objdump -p "$1" | grep -q NEEDED; }
  fi
  _has_dynamic_linking hello_c_d  || _err "hello_c_d is statically linked!"
  _has_dynamic_linking hello_cc_d || _err "hello_cc_d is statically linked!"
  if [ -n "$CC_TEST_STATIC" ]; then
    _has_dynamic_linking hello_c_s  && _err "hello_c_s has dynamic linking!"
    _has_dynamic_linking hello_cc_s && _err "hello_cc_s has dynamic linking!"
    echo "Verified static and dynamic linking with $LD_VERIFICATION_TOOL: OK"
  else
    echo "Verified dynamic linking with $LD_VERIFICATION_TOOL: OK"
  fi

  popd >/dev/null
  echo "All OK -- CC=$CC CXX=$CXX works correctly"
  echo "To re-run tests: rm ${DEPS_DIR#$PWD/*}/cc-tested && $0"
  touch $DEPS_DIR/cc-tested
fi


touch $DEPS_DIR/configured
