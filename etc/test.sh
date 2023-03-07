#!/usr/bin/env bash
set -euo pipefail
source "$(dirname "$0")/lib.sh"
_pushd "$PROJECT"

if [[ "$@" == *"-debug"* ]]; then
  co=out/debug/co ; ./build.sh -debug
else
  co=out/opt/co ; ./build.sh -no-lto
fi

mkdir -p out/test

cat << END > out/test/hello.c
#include <stdio.h>
int main(int argc, char* argv[]) {
  printf("Hello from %s\n", argv[0]);
  return 0;
}
END

# compile simple C program directly from source
$co cc out/test/hello.c -o out/test/hello.exe
out/test/hello.exe

# compile simple C program to object, then link executable
$co cc -c out/test/hello.c -o out/test/hello.o
$co cc out/test/hello.o -o out/test/hello.exe
out/test/hello.exe

# create object archive, then link executable
$co ar rcs out/test/libhello.a out/test/hello.o
$co cc out/test/libhello.a -o out/test/hello.exe
out/test/hello.exe

# link executable via -l
$co cc -Lout/test -lhello -o out/test/hello.exe
out/test/hello.exe
