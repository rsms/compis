# Build a C program with a custom --sysroot and -isysroot.
# Use wasi sysroot since it's small.
CFLAGS="--target=wasm32-wasi"

# query for effective sysroot
SYSROOT=$(
  c++ $CFLAGS -c hello.cc -### 2>/dev/null |
  grep -E '^sysroot=' |
  sed -E -e 's/^sysroot=(.+)/\1/')

echo "using custom sysroot: $SYSROOT"
[[ "$SYSROOT" == *"/" ]] && _err "SYSROOT ends with slash"

# build actual sysroot if needed
[ -d "$SYSROOT" ] || c++ $CFLAGS -c hello.cc

# try both "--sysroot=" and "--sysroot" command-line arg flavors
# since these take different code paths in cc.c
c++ $CFLAGS --sysroot="$SYSROOT" hello.cc -o hello.wasm
c++ $CFLAGS --sysroot "$SYSROOT" hello.cc -o hello.wasm
wasi-run hello.wasm

# test -isysroot
c++ $CFLAGS -isysroot "$SYSROOT" hello.cc -o hello.wasm
wasi-run hello.wasm
