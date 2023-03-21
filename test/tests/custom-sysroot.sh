# build a C program with a custom --sysroot

# use wasi sysroot since it's small
# Also, custom macos sysroot requires specifying -platform_version to linker
SYSROOT=$(
  cc --target=wasm32-wasi -v -c hello.c 2>/dev/null |
  grep -E '^sysroot=' |
  sed -E -e 's/^sysroot=(.+)/\1/')

echo "using custom sysroot: $SYSROOT"

# try both "--sysroot=" and "--sysroot" command-line arg flavors
# since these take different code paths in cc.c
cc --target=wasm32-wasi --sysroot="$SYSROOT" hello.c -o hello.wasm
cc --target=wasm32-wasi --sysroot "$SYSROOT" hello.c -o hello.wasm

if command -v wasmtime >/dev/null; then
  wasmtime hello.wasm
fi
