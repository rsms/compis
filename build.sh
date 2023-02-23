#!/bin/bash
set -euo pipefail
PWD0=$PWD
PROJECT=`cd "$(dirname "$0")"; pwd`

HOST_ARCH=$(uname -m) ; [ "$HOST_ARCH" != "arm64" ] || HOST_ARCH=aarch64
HOST_SYS=$(uname -s) # e.g. linux, macos
case "$HOST_SYS" in
  Darwin) HOST_SYS=macos ;;
  *)      HOST_SYS=$(awk '{print tolower($0)}' <<< "$HOST_SYS") ;;
esac

# —————————————————————————————————————————————————————————————————————————————————
# functions

_err() { echo "$0:" "$@" >&2; exit 1; }

_hascmd() { command -v "$1" >/dev/null || return 1; }
_needcmd() {
  while [ $# -gt 0 ]; do
    if ! _hascmd "$1"; then
      _err "missing $1 -- please install or use a different shell"
    fi
    shift
  done
}

_relpath() { # <path>
  case "$1" in
    "$PWD0/"*) echo "${1##$PWD0/}" ;;
    "$PWD0")   echo "." ;;
    "$HOME/"*) echo "~${1:${#HOME}}" ;;
    *)         echo "$1" ;;
  esac
}

_pushd() {
  pushd "$1" >/dev/null
  [ "$PWD" = "$PWD0" ] || echo "cd $(_relpath "$PWD")"
}

_popd() {
  popd >/dev/null
  [ "$PWD" = "$PWD0" ] || echo "cd $(_relpath "$PWD")"
}

_sha_verify() { # <file> [<sha256> | <sha512>]
  local file=$1
  local expect=$2
  local actual=
  echo "verifying checksum of $(_relpath "$file")"
  case "${#expect}" in
    128) kind=512; actual=$(sha512sum "$file" | cut -d' ' -f1) ;;
    64)  kind=256; actual=$(sha256sum "$file" | cut -d' ' -f1) ;;
    *)   _err "checksum $expect has incorrect length (not sha256 nor sha512)" ;;
  esac
  if [ "$actual" != "$expect" ]; then
    echo "$file: SHA-$kind sum mismatch:" >&2
    echo "  actual:   $actual" >&2
    echo "  expected: $expect" >&2
    return 1
  fi
}

_download_nocache() { # <url> <outfile> [<sha256> | <sha512>]
  local url=$1 ; local outfile=$2 ; local checksum=${3:-}
  rm -f "$outfile"
  mkdir -p "$(dirname "$outfile")"
  echo "$(_relpath "$outfile"): fetch $url"
  command -v wget >/dev/null &&
    wget -q --show-progress -O "$outfile" "$url" ||
    curl -L '-#' -o "$outfile" "$url"
  [ -z "$checksum" ] || _sha_verify "$outfile" "$checksum"
}

_download() { # <url> <outfile> [<sha256> | <sha512>]
  local url=$1 ; local outfile=$2 ; local checksum=${3:-}
  if [ -f "$outfile" ]; then
    [ -z "$checksum" ] && return 0
    _sha_verify "$outfile" "$checksum" && return 0
  fi
  _download_nocache "$url" "$outfile" "$checksum"
}

_extract_tar() { # <file> <outdir>
  [ $# -eq 2 ] || _err "_extract_tar"
  local tarfile=$1
  local outdir=$2
  [ -e "$tarfile" ] || _err "$tarfile not found"

  local extract_dir="${outdir%/}-extract-$(basename "$tarfile")"
  rm -rf "$extract_dir"
  mkdir -p "$extract_dir"

  echo "extract $(basename "$tarfile") -> $(_relpath "$outdir")"
  if ! XZ_OPT='-T0' tar -C "$extract_dir" -xf "$tarfile"; then
    rm -rf "$extract_dir"
    return 1
  fi
  rm -rf "$outdir"
  mkdir -p "$(dirname "$outdir")"
  mv -f "$extract_dir"/* "$outdir"
  rm -rf "$extract_dir"
}

# —————————————————————————————————————————————————————————————————————————————————
# command line

OUT_DIR_BASE="$PROJECT/out"
DEPS_DIR="$PROJECT/deps"
DOWNLOAD_DIR="$PROJECT/deps/download"
SRC_DIR="$PROJECT/src"
MAIN_EXE=co
PP_PREFIX=CO_
WASM_SYMS="$PROJECT/etc/wasm.syms"

WATCH=
WATCH_ADDL_FILES=()
RUN=
NON_WATCH_ARGS=()

OUT_DIR=
BUILD_MODE=opt  # opt | opt-fast | debug
TESTING_ENABLED=false
ONLY_CONFIGURE=false
STRIP=false
DEBUGGABLE=false
VERBOSE=false
ENABLE_LTO=
NINJA_ARGS=()
XFLAGS=()

while [[ $# -gt 0 ]]; do
  case "$1" in
    -w)      WATCH=1; shift; continue ;;
    -wf=*)   WATCH=1; WATCH_ADDL_FILES+=( "${1:4}" ); shift; continue ;;
    -run=*)  RUN=${1:5}; shift; continue ;;
  esac
  NON_WATCH_ARGS+=( "$1" )
  case "$1" in
  -opt)      BUILD_MODE=opt; shift ;;
  -opt-fast) BUILD_MODE=opt-fast; shift ;;
  -debug)    BUILD_MODE=debug; TESTING_ENABLED=true; DEBUGGABLE=true; shift ;;
  -config)   ONLY_CONFIGURE=true; shift ;;
  -strip)    STRIP=true; DEBUGGABLE=false; shift ;;
  -lto)      ENABLE_LTO=true; shift ;;
  -no-lto)   ENABLE_LTO=false; shift ;;
  -out=*)    OUT_DIR=${1:5}; shift; continue ;;
  -g)        DEBUGGABLE=true; STRIP=false; shift ;;
  -v)        VERBOSE=true; NINJA_ARGS+=(-v); shift ;;
  -D*)       [ ${#1} -gt 2 ] || _err "Missing NAME after -D";XFLAGS+=( "$1" );shift;;
  -h|-help|--help) cat << _END
usage: $0 [options] [--] [<target> ...]
Build mode option: (select just one)
  -opt           Build optimized product with some assertions enabled (default)
  -opt-fast      Build optimized product without any assertions
  -debug         Build debug product with full assertions and tracing capability
  -config        Just configure, only generate build.ninja file
Output options:
  -g             Make -opt build extra debuggable (basic opt only, frame pointers)
  -strip         Do not include debug data (negates -g)
  -lto           Enable LTO (default for -opt)
  -no-lto        Disable LTO (default for -debug)
  -out=<dir>     Build in <dir> instead of "$(_relpath "$OUT_DIR_BASE/<mode>")".
  -DNAME[=value] Define CPP variable NAME with value
Misc options:
  -w             Rebuild as sources change
  -wf=<file>     Watch <file> for changes (can be provided multiple times)
  -run=<cmd>     Run <cmd> after successful build
  -v             Verbose log messages and disables pretty ninja output
  -help          Show help on stdout and exit
_END
    exit ;;
  --) break ;;
  -*) _err "unknown option: $1" ;;
  *) break ;;
esac; done

DEBUG=false; [ "$BUILD_MODE" = "debug" ] && DEBUG=true

if [ -z "$ENABLE_LTO" -a "$BUILD_MODE" = "debug" ]; then
  ENABLE_LTO=false
elif [ -z "$ENABLE_LTO" ]; then
  ENABLE_LTO=true
fi

if [ -z "$OUT_DIR" ]; then
  OUT_DIR="$OUT_DIR_BASE/$BUILD_MODE"
else
  OUT_DIR=`cd "$OUT_DIR"; pwd`
fi

# —————————————————————————————————————————————————————————————————————————————————
# check availability of commands that we need
_needcmd head grep stat awk tar git ninja sha256sum
_hascmd curl || _hascmd wget || _err "curl nor wget found in PATH"

# —————————————————————————————————————————————————————————————————————————————————
# [dep] precompiled llvm from llvmbox distribution
LLVM_RELEASE=15.0.7
LLVMBOX_RELEASE=$LLVM_RELEASE+2
LLVMBOX_DESTDIR="$DEPS_DIR/llvmbox"
LLVMBOX_RELEASES=( # github.com/rsms/llvmbox/releases/download/VERSION/sha256sum.txt
  "fc7f24d4464127c91c76d2a7fed15c37137b00126acffadaa89912b372d0381a  llvmbox-15.0.7+1-aarch64-linux.tar.xz" \
  "3db9f9d42111207def5c70c16e0d473eaa65adeb89050976b431d5e806324316  llvmbox-15.0.7+1-aarch64-macos.tar.xz" \
  "3b859e76df8fcae7b8c7cf568af36b2d4eba88b8e403c9aa61320d6b77a47aff  llvmbox-15.0.7+1-x86_64-linux.tar.xz" \
  "f3c50460907c95a6aaae30c85b64a4fd76680c7180cfb35f35a5c135f791716e  llvmbox-15.0.7+1-x86_64-macos.tar.xz" \
  "eeeada6a30246202ef63a9c6677f38499ee638cad7cca5a3b25be91c9e7bccb7  llvmbox-dev-15.0.7+1-aarch64-linux.tar.xz" \
  "aee04221cc1fcc5c9a056a483a3a7392d7cd292baaff5ae3772ad507ed50093e  llvmbox-dev-15.0.7+1-aarch64-macos.tar.xz" \
  "8bb26eb983e47ac74a3393593eebd24242f3a8fd8b37de21dbca6709ec7968fe  llvmbox-dev-15.0.7+1-x86_64-linux.tar.xz" \
  "bd508ddcfe52fee3ffa6fae47c30a1eba1d48678e3a6c5da333275de9f22a236  llvmbox-dev-15.0.7+1-x86_64-macos.tar.xz" \
)
LLVMBOX_URL_BASE=https://github.com/rsms/llvmbox/releases/download/v$LLVMBOX_RELEASE
LLVM_CONFIG="$LLVMBOX_DESTDIR/bin/llvm-config"

# find tar for host system
LLVMBOX_SHA256_FILE=
LLVMBOX_DEV_SHA256_FILE=
for sha256_file in "${LLVMBOX_RELEASES[@]}"; do
  IFS=' ' read -r sha256 file <<< "$sha256_file"
  if [[ "$file" == "llvmbox-$LLVMBOX_RELEASE-$HOST_ARCH-$HOST_SYS.tar.xz" ]]; then
    LLVMBOX_SHA256_FILE=$sha256_file
  elif [[ "$file" == "llvmbox-dev-$LLVMBOX_RELEASE-$HOST_ARCH-$HOST_SYS.tar.xz" ]]; then
    LLVMBOX_DEV_SHA256_FILE=$sha256_file
  fi
done
[ -n "$LLVMBOX_SHA256_FILE" ] ||
  _err "llvmbox not available for llvm-$LLVMBOX_RELEASE-$HOST_ARCH-$HOST_SYS"
[ -n "$LLVMBOX_DEV_SHA256_FILE" ] ||
  _err "llvmbox-dev not available for llvm-$LLVMBOX_RELEASE-$HOST_ARCH-$HOST_SYS"

# extract llvmbox
if [ "$(cat "$LLVMBOX_DESTDIR/version" 2>/dev/null)" != "$LLVMBOX_RELEASE" ]; then
  IFS=' ' read -r sha256 file <<< "$LLVMBOX_SHA256_FILE"
  _download "$LLVMBOX_URL_BASE/$file" "$DOWNLOAD_DIR/$file" "$sha256"
  _extract_tar "$DOWNLOAD_DIR/$file" "$LLVMBOX_DESTDIR"
  echo "$LLVMBOX_RELEASE" > "$LLVMBOX_DESTDIR/version"
else
  $VERBOSE && echo "$(_relpath "$LLVMBOX_DESTDIR") (base): up-to-date"
fi

# extract llvmbox-dev into llvmbox
if [ ! -f "$LLVMBOX_DESTDIR/lib/libz.a" ]; then
  IFS=' ' read -r sha256 file <<< "$LLVMBOX_DEV_SHA256_FILE"
  _download "$LLVMBOX_URL_BASE/$file" "$DOWNLOAD_DIR/$file" "$sha256"
  echo "extract $(basename "$DOWNLOAD_DIR/$file") -> $(_relpath "$LLVMBOX_DESTDIR")"
  XZ_OPT='-T0' tar -C "$LLVMBOX_DESTDIR" --strip-components 1 -xf "$DOWNLOAD_DIR/$file"
else
  $VERBOSE && echo "$(_relpath "$LLVMBOX_DESTDIR") (dev): up-to-date"
fi

export CC="$LLVMBOX_DESTDIR/bin/clang"
export CXX="$LLVMBOX_DESTDIR/bin/clang++"
export PATH="$LLVMBOX_DESTDIR/bin:$PATH"

# —————————————————————————————————————————————————————————————————————————————————
# update clang driver code if needed

SRC_VERSION_LINE="//!llvm-$LLVM_RELEASE"

if [ "$(tail -n1 "$SRC_DIR/llvm/driver.cc")" != "$SRC_VERSION_LINE" ]; then
  echo "src/llvm: LLVM version changed; updating driver code"

  _download \
    https://github.com/llvm/llvm-project/releases/download/llvmorg-$LLVM_RELEASE/clang-$LLVM_RELEASE.src.tar.xz \
    "$DOWNLOAD_DIR/clang-$LLVM_RELEASE.src.tar.xz" \
    a6b673ef15377fb46062d164e8ddc4d05c348ff8968f015f7f4af03f51000067

  _extract_tar "$DOWNLOAD_DIR/clang-$LLVM_RELEASE.src.tar.xz" "$DEPS_DIR/clang"

  _pushd "$SRC_DIR"
  cp -v "$DEPS_DIR/clang/tools/driver/driver.cpp"     llvm/driver.cc
  cp -v "$DEPS_DIR/clang/tools/driver/cc1_main.cpp"   llvm/driver_cc1_main.cc
  cp -v "$DEPS_DIR/clang/tools/driver/cc1as_main.cpp" llvm/driver_cc1as_main.cc
  patch -p1 < "$PROJECT/etc/co-llvm-$LLVM_RELEASE-driver.patch"
  echo "$SRC_VERSION_LINE" >> llvm/driver.cc
  _popd

  rm -rf "$DEPS_DIR/clang" "$DOWNLOAD_DIR/clang-$LLVM_RELEASE.src.tar.xz"
fi

# —————————————————————————————————————————————————————————————————————————————————
# file system watcher

# -w to enter "watch & build & run" mode
if [ -n "$WATCH" ]; then
  WATCH_TOOL=fswatch
  # note: inotifywait is part of inotify-tools
  command -v $WATCH_TOOL >/dev/null || WATCH_TOOL=inotifywait
  command -v $WATCH_TOOL >/dev/null ||
    _err "no watch tool available (looked for fswatch and inotifywait in PATH)"

  _fswatch() {
    if [ "$WATCH_TOOL" = fswatch ]; then
      fswatch --one-event --extended --latency=0.1 \
        --exclude='\.(a|o|d)$' --recursive "$@"
    else
      inotifywait -e modify -e create -e delete -e move -qq \
        --exclude='\.(a|o|d)$' --recursive "$@"
    fi
  }

  # case "$RUN" in
  #   *" "*) RUN="$SHELL -c '$RUN'" ;;
  # esac
  RUN_PIDFILE=${TMPDIR:-/tmp}/$(basename "$(dirname "$PWD")").build-runpid.$$
  echo "RUN_PIDFILE=$RUN_PIDFILE"
  _killcmd() {
    local RUN_PID=$(cat "$RUN_PIDFILE" 2>/dev/null)
    if [ -n "$RUN_PID" ]; then
      kill $RUN_PID 2>/dev/null && echo "killing #$RUN_PID"
      ( sleep 0.1 ; kill -9 "$RUN_PID" 2>/dev/null || true ) &
      rm -f "$RUN_PIDFILE"
    fi
  }
  _exit() {
    _killcmd
    kill $(jobs -p) 2>/dev/null || true
    rm -f "$RUN_PIDFILE"
    exit
  }
  trap _exit SIGINT  # make sure we can ctrl-c in the while loop
  while true; do
    printf "\x1bc"  # clear screen ("scroll to top" style)
    BUILD_OK=1
    bash "./$(basename "$0")" ${NON_WATCH_ARGS[@]:-} "$@" || BUILD_OK=
    printf "\e[2m> watching files for changes...\e[m\n"
    if [ -n "$BUILD_OK" -a -n "$RUN" ]; then
      export ASAN_OPTIONS=detect_stack_use_after_return=1
      export UBSAN_OPTIONS=print_stacktrace=1
      _killcmd
      ( "${BASH:-bash}" -c "$RUN" &
        RUN_PID=$!
        echo $RUN_PID > "$RUN_PIDFILE"
        echo "$RUN (#$RUN_PID) started"
        wait
        # TODO: get exit code from $RUN
        # Some claim wait sets the exit code, but not in my bash.
        # The idea would be to capture exit code from wait:
        #   status=$?
        echo "$RUN (#$RUN_PID) exited"
      ) &
    fi
    _fswatch "$SRC_DIR" "$(basename "$0")" ${WATCH_ADDL_FILES[@]:-}
  done
  exit 0
fi

# —————————————————————————————————————————————————————————————————————————————————
# construct flags
#
#   XFLAGS             compiler flags (common to C and C++)
#     XFLAGS_HOST      compiler flags specific to native host target
#     XFLAGS_WASM      compiler flags specific to WASM target
#     CFLAGS           compiler flags for C
#       CFLAGS_HOST    compiler flags for C specific to native host target
#       CFLAGS_WASM    compiler flags for C specific to WASM target
#       CFLAGS_LLVM    compiler flags for C files in src/llvm/
#     CXXFLAGS         compiler flags for C++
#       CXXFLAGS_HOST  compiler flags for C++ specific to native host target
#       CXXFLAGS_WASM  compiler flags for C++ specific to WASM target
#       CXXFLAGS_LLVM  compiler flags for C++ files in src/llvm/
#   LDFLAGS            linker flags common to all targets
#     LDFLAGS_HOST     linker flags specific to native host target (cc suite's ld)
#     LDFLAGS_WASM     linker flags specific to WASM target (llvm's wasm-ld)
#
XFLAGS+=(
  -g \
  -feliminate-unused-debug-types \
  -fvisibility=hidden \
  -Wall -Wextra -Wvla \
  -Wimplicit-fallthrough \
  -Wno-missing-field-initializers \
  -Wno-unused-parameter \
  -Werror=implicit-function-declaration \
  -Werror=incompatible-pointer-types \
  -Werror=int-conversion \
  -Werror=format \
  -Wcovered-switch-default \
  -Werror=format-insufficient-args \
  -Werror=bitfield-constant-conversion \
  -Wno-pragma-once-outside-header \
)
[ -t 1 ] && XFLAGS+=( -fcolor-diagnostics )
$TESTING_ENABLED && XFLAGS+=( -D${PP_PREFIX}TESTING_ENABLED )
XFLAGS_HOST=()
XFLAGS_WASM=( --target=wasm32 -fvisibility=hidden )

CFLAGS=( -std=c11 -fms-extensions -Wno-microsoft )
CFLAGS_HOST=( $("$LLVM_CONFIG" --cflags) )
CFLAGS_WASM=()
CFLAGS_LLVM=()

CXXFLAGS=( -std=c++14 -fvisibility-inlines-hidden -fno-exceptions -fno-rtti )
CXXFLAGS_HOST=()
CXXFLAGS_WASM=()
CXXFLAGS_LLVM=()

LDFLAGS_HOST=( -gz=zlib )
LDFLAGS_WASM=( --no-entry --no-gc-sections --export-dynamic --import-memory )

# arch-and-system-specific flags
case "$HOST_ARCH-$HOST_SYS" in
  x86_64-macos)  LDFLAGS_HOST+=( -Wl,-platform_version,macos,10.15,10.15 ) ;;
  aarch64-macos) LDFLAGS_HOST+=( -Wl,-platform_version,macos,11.0,11.0 ) ;;
esac

# system-specific flags
case "$HOST_SYS" in
  linux) LDFLAGS_HOST+=( -static ) ;;
esac

# build-mode-specific flags
case "$BUILD_MODE" in
  debug)
    XFLAGS+=( -O0 -DDEBUG -ferror-limit=6 )
    # Enable sanitizers in debug builds
    # See https://clang.llvm.org/docs/AddressSanitizer.html
    # See https://clang.llvm.org/docs/UndefinedBehaviorSanitizer.html
    XFLAGS_HOST+=(
      -fsanitize=address,undefined \
      -fsanitize-address-use-after-scope \
      -fsanitize=float-divide-by-zero \
      -fsanitize=null \
      -fsanitize=nonnull-attribute \
      -fsanitize=nullability \
      -fno-omit-frame-pointer \
      -fno-optimize-sibling-calls \
      -fmacro-backtrace-limit=0 \
    )
    LDFLAGS_HOST+=( -fsanitize=address,undefined )
    ;;
  opt*)
    XFLAGS+=( -DNDEBUG )
    XFLAGS_WASM+=( -Oz )
    if $DEBUGGABLE; then
      XFLAGS_HOST+=( -O1 -fno-omit-frame-pointer -fno-optimize-sibling-calls )
    else
      XFLAGS_HOST+=( -O3 -fomit-frame-pointer )
    fi
    # LDFLAGS_WASM+=( -z stack-size=$[128 * 1024] ) # larger stack, smaller heap
    # LDFLAGS_WASM+=( --compress-relocations --strip-debug )
    # LDFLAGS_HOST+=( -dead_strip )
    [ "$BUILD_MODE" = opt ] && XFLAGS+=( -D${PP_PREFIX}SAFE )
    ;;
esac

# LTO
if $ENABLE_LTO; then
  XFLAGS+=( -flto=thin )
  LDFLAGS_HOST+=( -flto=thin -Wl,--lto-O3 )
  LDFLAGS_WASM+=( -flto=thin --lto-O3 --no-lto-legacy-pass-manager )
  LTO_CACHE_FLAG=
  case "$HOST_SYS" in
    linux) LTO_CACHE_FLAG=-Wl,--thinlto-cache-dir="'$OUT_DIR/lto-cache'" ;;
    macos) LTO_CACHE_FLAG=-Wl,-cache_path_lto,"'$OUT_DIR/lto-cache'" ;;
  esac
  LDFLAGS_HOST+=( $LTO_CACHE_FLAG )
  LDFLAGS_WASM+=( $LTO_CACHE_FLAG )
fi

# llvm
# CFLAGS_LLVM+=( $("$LLVM_CONFIG" --cflags) )
CFLAGS_LLVM+=()
CXXFLAGS_LLVM+=( $("$LLVM_CONFIG" --cxxflags) )
if $ENABLE_LTO; then
  LDFLAGS_HOST+=( -L"$LLVMBOX_DESTDIR"/lib-lto )
  for f in "$LLVMBOX_DESTDIR"/lib-lto/lib{clang,lld,LLVM}*.a; do
    f="$(basename "$f" .a)"
    LDFLAGS_HOST+=( -l${f:3} )
  done
else
  LDFLAGS_HOST+=( -L"$LLVMBOX_DESTDIR"/lib -lall_llvm_clang_lld )
fi
LDFLAGS_HOST+=( $("$LLVM_CONFIG" --system-libs) )

#————————————————————————————————————————————————————————————————————————————————————————
# find source files
#
# name.{c,cc}       always included
# name.ARCH.{c,cc}  specific to ARCH (e.g. wasm, x86_64, aarch64, etc)
# name.test.{c,cc}  only included when testing is enabled

COMMON_SOURCES=()
HOST_SOURCES=()
WASM_SOURCES=()
TEST_SOURCES=()

pushd "$PROJECT" >/dev/null
SRC_DIR_REL="${SRC_DIR##$PROJECT/}"
for f in $(find "$SRC_DIR_REL" -name '*.c' -or -name '*.cc'); do
  case "$f" in
    */test.c|*.test.c|*.test.cc)    TEST_SOURCES+=( "$f" ) ;;
    *.$HOST_ARCH.c|*.$HOST_ARCH.cc) HOST_SOURCES+=( "$f" ) ;;
    *.wasm.c|*.wasm.cc)             WASM_SOURCES+=( "$f" ) ;;
    *)                              COMMON_SOURCES+=( "$f" ) ;;
  esac
done
popd >/dev/null

#————————————————————————————————————————————————————————————————————————————————————————
# generate .clang_complete

echo "-I$SRC_DIR" > .clang_complete
for flag in \
  "${XFLAGS[@]:-}" \
  "${XFLAGS_HOST[@]:-}" \
  "${CFLAGS_HOST[@]:-}" \
  "${CFLAGS_LLVM[@]:-}" \
;do
  [ -n "$flag" ] && echo "$flag" >> .clang_complete
done

# —————————————————————————————————————————————————————————————————————————————————
# print config when -v is set

$VERBOSE && cat << END
XFLAGS            ${XFLAGS[@]:-}
  XFLAGS_HOST     ${XFLAGS_HOST[@]:-}
  XFLAGS_WASM     ${XFLAGS_WASM[@]:-}
  CFLAGS          ${CFLAGS[@]:-}
    CFLAGS_HOST   ${CFLAGS_HOST[@]:-}
    CFLAGS_WASM   ${CFLAGS_WASM[@]:-}
    CFLAGS_LLVM   ${CFLAGS_LLVM[@]:-}
  CXXFLAGS        ${CXXFLAGS[@]:-}
    CXXFLAGS_HOST ${CXXFLAGS_HOST[@]:-}
    CXXFLAGS_WASM ${CXXFLAGS_WASM[@]:-}
    CXXFLAGS_LLVM ${CXXFLAGS_LLVM[@]:-}

LDFLAGS        ${LDFLAGS[@]:-}
  LDFLAGS_HOST ${LDFLAGS_HOST[@]:-}
  LDFLAGS_WASM ${LDFLAGS_WASM[@]:-}

TEST_SOURCES   ${TEST_SOURCES[@]:-}
HOST_SOURCES   ${HOST_SOURCES[@]:-}
WASM_SOURCES   ${WASM_SOURCES[@]:-}
COMMON_SOURCES ${COMMON_SOURCES[@]:-}
END

#————————————————————————————————————————————————————————————————————————————————————————
# generate build.ninja

mkdir -p "$OUT_DIR/obj"
cd "$OUT_DIR"

NF=build.ninja.tmp
cat << _END > $NF
ninja_required_version = 1.3
builddir = $OUT_DIR
objdir = \$builddir/obj

xflags = ${XFLAGS[@]:-}
xflags_host = \$xflags ${XFLAGS_HOST[@]:-}
xflags_wasm = \$xflags ${XFLAGS_WASM[@]:-}

cflags = ${CFLAGS[@]:-}
cflags_host = \$xflags_host ${CFLAGS_HOST[@]:-}
cflags_wasm = \$xflags_wasm ${CFLAGS_WASM[@]:-}
cflags_llvm = ${CFLAGS_LLVM[@]:-}

cxxflags = ${CXXFLAGS[@]:-}
cxxflags_host = \$xflags_host ${CXXFLAGS_HOST[@]:-}
cxxflags_wasm = \$xflags_wasm ${CXXFLAGS_WASM[@]:-}
cxxflags_llvm = ${CXXFLAGS_LLVM[@]:-}

ldflags_host = ${LDFLAGS_HOST[@]:-}
ldflags_wasm = ${LDFLAGS_WASM[@]:-}


rule link
  command = $CXX \$ldflags_host \$FLAGS -o \$out \$in
  description = link \$out

rule link_wasm
  command = wasm-ld \$ldflags_wasm \$FLAGS \$in -o \$out
  description = link \$out


rule cc
  command = $CC -MMD -MF \$out.d \$cflags \$cflags_host \$FLAGS -c \$in -o \$out
  depfile = \$out.d
  description = compile \$in

rule cxx
  command = $CXX -MMD -MF \$out.d \$cxxflags \$cxxflags_host \$FLAGS -c \$in -o \$out
  depfile = \$out.d
  description = compile \$in


rule cc_wasm
  command = $CC -MMD -MF \$out.d \$cflags \$cflags_wasm \$FLAGS -c \$in -o \$out
  depfile = \$out.d
  description = compile \$in

rule cxx_wasm
  command = $CXX -MMD -MF \$out.d \$cxxflags \$cxxflags_wasm \$FLAGS -c \$in -o \$out
  depfile = \$out.d
  description = compile \$in


rule ast_gen
  command = python3 src/parse/ast_gen.py \$in \$out
  generator = true

rule parse_gen
  command = python3 src/parse/parse_gen.py \$in \$out
  generator = true

rule cxx_pch_gen
  command = $CXX \$cxxflags \$cxxflags_host \$FLAGS -x c++-header \$in -o \$out
  #clang -cc1 -emit-pch -x c++-header \$in -o \$out
  description = compile-pch \$in
  generator = true

build src/parse/ast_gen.h src/parse/ast_gen.c: ast_gen src/parse/ast.h | src/parse/ast_gen.py
build src/parse/parser_gen.h: parse_gen src/parse/parser.c | src/parse/parse_gen.py

build \$objdir/llvm-includes.pch: cxx_pch_gen src/llvm/llvm-includes.hh
  FLAGS = \$cxxflags_llvm

_END


_objfile() {
  echo \$objdir/${1//\//.}.o
}

_gen_obj_build_rules() { # <target> <srcfile> ...
  local target=$1 ; shift
  local srcfile
  local objfile
  local cc_rule=cc
  local cxx_rule=cxx
  if [ "$target" = wasm ]; then
    cc_rule=cc_wasm
    cxx_rule=cxx_wasm
  fi
  for srcfile in "$@"; do
    [ -n "$srcfile" ] || continue
    objfile=$(_objfile "$target-$srcfile")
    case "$srcfile" in
      */llvm/*.c)
        echo "build $objfile: $cc_rule $srcfile" >> $NF
        echo "  FLAGS = \$cflags_llvm" >> $NF
        ;;
      */llvm/*.cc)
        echo "build $objfile: $cxx_rule $srcfile | \$objdir/llvm-includes.pch" >> $NF
        echo "  FLAGS = -include-pch \$objdir/llvm-includes.pch \$cxxflags_llvm" >> $NF
        ;;
      *.c)
        echo "build $objfile: $cc_rule $srcfile" >> $NF
        ;;
      *.cc)
        echo "build $objfile: $cxx_rule $srcfile" >> $NF
        ;;
      *)
        _err "don't know how to compile file type: \"$srcfile\""
        ;;
    esac
    echo "$objfile"
  done
}

HOST_SOURCES+=( "${COMMON_SOURCES[@]:-}" )
WASM_SOURCES+=( "${COMMON_SOURCES[@]:-}" )

if $TESTING_ENABLED; then
  HOST_SOURCES+=( "${TEST_SOURCES[@]:-}" )
  WASM_SOURCES+=( "${TEST_SOURCES[@]:-}" )
fi

OBJECTS=( $(_gen_obj_build_rules "host" "${HOST_SOURCES[@]:-}") )
if [ ${#OBJECTS[@]} ]; then
  echo >> $NF
  echo "build \$builddir/$MAIN_EXE: link ${OBJECTS[@]}" >> $NF
  echo >> $NF
fi

OBJECTS=( $(_gen_obj_build_rules "wasm" "${WASM_SOURCES[@]:-}") )
if [ ${#OBJECTS[@]} ]; then
  echo >> $NF
  echo "build \$builddir/$MAIN_EXE.wasm: link_wasm ${OBJECTS[@]}" >> $NF
  echo >> $NF
fi

echo "build $MAIN_EXE: phony \$builddir/$MAIN_EXE" >> $NF
echo "build $MAIN_EXE.wasm: phony \$builddir/$MAIN_EXE.wasm" >> $NF
echo "default $MAIN_EXE" >> $NF

# —————————————————————————————————————————————————————————————————————————————————
# write build.ninja if it changed
NINJAFILE=build.ninja
NINJA_ARGS+=( -f "$OUT_DIR/build.ninja" )

if [ "$(sha256sum $NF | cut -d' ' -f1)" != \
     "$(sha256sum build.ninja 2>/dev/null | cut -d' ' -f1)" ]
then
  mv $NF build.ninja
  $VERBOSE && echo "$(_relpath "$PWD/build.ninja") updated"
else
  rm $NF
  $VERBOSE && echo "$(_relpath "$PWD/build.ninja") is up-to-date"
fi

# stop now if -config is set
$ONLY_CONFIGURE && exit

# —————————————————————————————————————————————————————————————————————————————————
# wipe build cache if config or tools changed

cat << END > config.tmp
TARGET $HOST_ARCH-$HOST_SYS
LLVM $LLVMBOX_RELEASE
XFLAGS ${XFLAGS[@]:-}
XFLAGS_HOST ${XFLAGS_HOST[@]:-}
XFLAGS_WASM ${XFLAGS_WASM[@]:-}
CFLAGS ${CFLAGS[@]:-}
CFLAGS_HOST ${CFLAGS_HOST[@]:-}
CFLAGS_WASM ${CFLAGS_WASM[@]:-}
CFLAGS_LLVM ${CFLAGS_LLVM[@]:-}
CXXFLAGS ${CXXFLAGS[@]:-}
CXXFLAGS_HOST ${CXXFLAGS_HOST[@]:-}
CXXFLAGS_WASM ${CXXFLAGS_WASM[@]:-}
CXXFLAGS_LLVM ${CXXFLAGS_LLVM[@]:-}
LDFLAGS ${LDFLAGS[@]:-}
LDFLAGS_HOST ${LDFLAGS_HOST[@]:-}
LDFLAGS_WASM ${LDFLAGS_WASM[@]:-}
END
if ! diff -q config config.tmp >/dev/null 2>&1; then
  [ -e config ] && echo "build configuration changed"
  mv config.tmp config
  rm -rf "$OUT_DIR/obj" "$OUT_DIR/lto-cache"
else
  rm config.tmp
fi

# —————————————————————————————————————————————————————————————————————————————————
# run ninja

cd "$PROJECT"
if [ -n "$RUN" ]; then
  ninja "${NINJA_ARGS[@]}" "$@"
  echo $RUN
  exec "${BASH:-bash}" -c "$RUN"
fi
exec ninja "${NINJA_ARGS[@]}" "$@"
