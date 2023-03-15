#!/bin/bash
set -euo pipefail
PWD0=$PWD
PROJECT=`cd "$(dirname "$0")"; pwd`
source "$PROJECT/etc/lib.sh"

# —————————————————————————————————————————————————————————————————————————————————
# command line

OUT_DIR_BASE="$PROJECT/out"
DEPS_DIR="$PROJECT/deps"
DOWNLOAD_DIR="$PROJECT/deps/download"
SRC_DIR="$PROJECT/src"
MAIN_EXE=co
PP_PREFIX=CO_
WASM_SYMS="$PROJECT/etc/wasm.syms"
TARGET=

WATCH=
WATCH_ADDL_FILES=()
RUN=
NON_WATCH_ARGS=()

OUT_DIR=
BUILD_MODE=opt  # opt | opt-fast | debug
TESTING_ENABLED=false
ONLY_CONFIGURE=false
STRIP=false
DEBUGGABLE=true
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
  -target=*) TARGET=${1:8}; shift; continue ;;
  -g)        DEBUGGABLE=true; STRIP=false; shift ;;
  -v)        VERBOSE=true; NINJA_ARGS+=(-v); shift ;;
  -n)        NINJA_ARGS+=(-n); shift ;;
  -D*)       [ ${#1} -gt 2 ] || _err "Missing NAME after -D";XFLAGS+=( "$1" );shift;;
  -h|-help|--help) cat << _END
usage: $0 [options] [--] [<target> ...]
Build mode option: (select just one)
  -opt             Build optimized product with some assertions enabled (default)
  -opt-fast        Build optimized product with no assertions
  -debug           Build debug product with full assertions and tracing capability
  -config          Just configure, only generate build.ninja file
  -target=<target> Build for <target> (e.g. x86_64-macos, aarch64-linux)
Output options:
  -g             Make -opt-fast debuggable (default for -opt and -debug)
  -strip         Do not include debug data (negates -g)
  -lto           Enable LTO (default for -opt)
  -no-lto        Disable LTO (default for -debug)
  -out=<dir>     Build in <dir> instead of "$(_relpath "$OUT_DIR_BASE/<mode>")".
  -DNAME[=value] Define CPP variable NAME with value
Misc options:
  -w         Rebuild as sources change
  -wf=<file> Watch <file> for changes (can be provided multiple times)
  -run=<cmd> Run <cmd> after successful build
  -v         Verbose log messages and disables pretty ninja output
  -help      Show help on stdout and exit
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

# —————————————————————————————————————————————————————————————————————————————————
# target

HOST_ARCH=$(uname -m); HOST_ARCH=${HOST_ARCH/arm64/aarch64}
HOST_SYS=$(uname -s) # e.g. linux, macos
case "$HOST_SYS" in
  Darwin) HOST_SYS=macos ;;
  *)      HOST_SYS=$(awk '{print tolower($0)}' <<< "$HOST_SYS") ;;
esac

# cross compilation
TARGET_ARCH=$HOST_ARCH
TARGET_SYS=$HOST_SYS
if [ -n "$TARGET" ]; then
  [[ "$TARGET" != *"."* ]] || _err "unexpected version in target \"$TARGET\""
  for target_info in $(_co_targets); do
    # arch,sys,sysver,intsize,ptrsize,llvm_triple
    IFS=, read -r arch sys sysver intsize ptrsize llvm_triple <<< "$target_info"
    if [ "$TARGET" = "$arch-$sys" ]; then
      TARGET="$arch-$sys"
      TARGET_ARCH=$arch
      TARGET_SYS=$sys
      TARGET_TRIPLE=$llvm_triple
      break
    fi
  done
  if [ -z "$TARGET_TRIPLE" ]; then
    echo "$0: Invalid target: $TARGET" >&2
    printf "Available targets:" >&2
    for target_info in $(_co_targets); do
      IFS=, read -r arch sys sysver ign <<< "$target_info"
      [ -n "$sysver" ] && printf " $arch-$sys.$sysver" || printf " $arch-$sys"
    done >&2
    echo >&2
    exit 1
  fi
  $VERBOSE && echo "TARGET=$TARGET"
  $VERBOSE && echo "TARGET_TRIPLE=$TARGET_TRIPLE"
fi

# —————————————————————————————————————————————————————————————————————————————————
# OUT_DIR

if [ -z "$OUT_DIR" ]; then
  OUT_DIR="$OUT_DIR_BASE/$BUILD_MODE"
  test $TARGET && OUT_DIR="$OUT_DIR-$TARGET"
  mkdir -p "$OUT_DIR"
else
  mkdir -p "$OUT_DIR"
  OUT_DIR=`cd "$OUT_DIR"; pwd`
fi

# —————————————————————————————————————————————————————————————————————————————————
# check availability of commands that we need
_needcmd head grep stat awk tar git sha256sum
_hascmd curl || _hascmd wget || _err "curl nor wget found in PATH"

# —————————————————————————————————————————————————————————————————————————————————
# llvmbox

LLVM_RELEASE=15.0.7
LLVMBOX_RELEASE=$LLVM_RELEASE+3
LLVMBOX_DESTDIR="$DEPS_DIR/llvmbox"
LLVMBOX_RELEASES=( # github.com/rsms/llvmbox/releases/download/VERSION/sha256sum.txt
  "b7cc09f1864be3c2c2dca586224c932082638c8c6d60ca9e92e29564b729eb3e  llvmbox-15.0.7+3-aarch64-linux.tar.xz" \
  "2df11c8106d844957ef49997c07d970eb5730a964586dd20f7c155aa9409376f  llvmbox-15.0.7+3-aarch64-macos.tar.xz" \
  "672bf8d94228880ece00082794936514f97cd50e23c1b5045ed06db4b4f80333  llvmbox-15.0.7+3-x86_64-linux.tar.xz" \
  "a508cf2ef7199726f041e4ae0e92650636a4fc14ba1f37b40ae9694b198d0785  llvmbox-15.0.7+3-x86_64-macos.tar.xz" \
  "a1603875d1f9a5eb327a596266c1f10b3c6be8f50e1313216c9924e5415284e5  llvmbox-dev-15.0.7+3-aarch64-linux.tar.xz" \
  "b339ad359e52ce9cd6cddcd68a908412a1d178dd5848710906621c96f5d6b41e  llvmbox-dev-15.0.7+3-aarch64-macos.tar.xz" \
  "513b49be901c3502e28e17e6748cc350dfd35a0261faae9a84256b07748799db  llvmbox-dev-15.0.7+3-x86_64-linux.tar.xz" \
  "746dd6fb68fe2dac217de3e81cf048829530af4c5b4f65fffb36e404d21a62bd  llvmbox-dev-15.0.7+3-x86_64-macos.tar.xz" \
)
LLVMBOX_URL_BASE=https://github.com/rsms/llvmbox/releases/download/v$LLVMBOX_RELEASE
LLVM_CONFIG="$LLVMBOX_DESTDIR/bin/llvm-config"

# find llvmbox for host system
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

# cross compilation: extract llvmbox-dev into dedicated directory
if test $TARGET && [ "$TARGET_ARCH-$TARGET_SYS" != "$HOST_ARCH-$HOST_SYS" ]; then
  TARGET_LLVMBOX_DEV="$LLVMBOX_DESTDIR-$TARGET"
  LLVMBOX_DEV_SHA256_FILE=
  for sha256_file in "${LLVMBOX_RELEASES[@]}"; do
    IFS=' ' read -r sha256 tar_file <<< "$sha256_file"
    if [ "$tar_file" = "llvmbox-dev-$LLVMBOX_RELEASE-$TARGET_ARCH-$TARGET_SYS.tar.xz" ]
    then
      LLVMBOX_DEV_SHA256_FILE=$sha256_file
      break
    fi
  done
  [ -n "$LLVMBOX_DEV_SHA256_FILE" ] ||
    _err "unsupported target $TARGET (no llvmbox distribution found for this target)"
  if [ ! -f "$TARGET_LLVMBOX_DEV/lib/libz.a" ]; then
    _download "$LLVMBOX_URL_BASE/$tar_file" "$DOWNLOAD_DIR/$tar_file" "$sha256"
    _extract_tar "$DOWNLOAD_DIR/$tar_file" "$TARGET_LLVMBOX_DEV"
  else
    $VERBOSE && echo "$(_relpath "$TARGET_LLVMBOX_DEV"): up-to-date"
  fi
fi

NINJA=${NINJA:-$(command -v ninja 2>/dev/null || true)}
export CC="$LLVMBOX_DESTDIR/bin/clang"
export CXX="$LLVMBOX_DESTDIR/bin/clang++"
export PATH="$LLVMBOX_DESTDIR/bin:$PATH"
[ -n "$NINJA" ] || NINJA=$LLVMBOX_DESTDIR/bin/ninja
export NINJA  # for watch mode

# —————————————————————————————————————————————————————————————————————————————————
# update llvm-derived code, if needed

SRC_VERSION_LINE="//!co-llvm-$LLVM_RELEASE"
LLVMSRC="$DEPS_DIR/llvm-$LLVM_RELEASE"
CLANGSRC="$DEPS_DIR/clang-$LLVM_RELEASE"

_require_llvm_src() {
  [ -d "$LLVMSRC" ] || _download_and_extract_tar \
    https://github.com/llvm/llvm-project/releases/download/llvmorg-$LLVM_RELEASE/llvm-$LLVM_RELEASE.src.tar.xz \
    "$LLVMSRC" \
    4ad8b2cc8003c86d0078d15d987d84e3a739f24aae9033865c027abae93ee7a4
}

if [ "$(tail -n1 "$SRC_DIR/llvm/clang.cc")" != "$SRC_VERSION_LINE" ]; then
  echo "updating src/llvm/clang.cc et al"

  [ -d "$CLANGSRC" ] || _download_and_extract_tar \
    https://github.com/llvm/llvm-project/releases/download/llvmorg-$LLVM_RELEASE/clang-$LLVM_RELEASE.src.tar.xz \
    "$CLANGSRC"
    a6b673ef15377fb46062d164e8ddc4d05c348ff8968f015f7f4af03f51000067

  _pushd "$PROJECT"
  _copy "$CLANGSRC/tools/driver/driver.cpp"     src/llvm/clang.cc
  _copy "$CLANGSRC/tools/driver/cc1_main.cpp"   src/llvm/clang_cc1_main.cc
  _copy "$CLANGSRC/tools/driver/cc1as_main.cpp" src/llvm/clang_cc1as_main.cc
  patch -p1 < etc/co-clang-$LLVM_RELEASE-driver.patch
  echo "$SRC_VERSION_LINE" >> src/llvm/clang.cc
  _popd
fi

DSTFILE="$SRC_DIR/llvm/llvm-ar.cc"
if [ "$(tail -n1 "$DSTFILE")" != "$SRC_VERSION_LINE" ]; then
  echo "updating $(_relpath "$DSTFILE")"
  _require_llvm_src
  _pushd "$PROJECT"
  _copy "$LLVMSRC/tools/llvm-ar/llvm-ar.cpp" "$DSTFILE"
  patch -p1 < etc/co-llvm-$LLVM_RELEASE-ar.patch
  echo "$SRC_VERSION_LINE" >> "$DSTFILE"
  _popd
fi

DSTFILE="$SRC_DIR/llvm/llvm-nm.cc"
if [ "$(tail -n1 "$DSTFILE")" != "$SRC_VERSION_LINE" ]; then
  echo "updating $(_relpath "$DSTFILE")"
  _require_llvm_src
  _pushd "$PROJECT"
  _copy "$LLVMSRC/tools/llvm-nm/llvm-nm.cpp" "$DSTFILE"
  patch -p1 < etc/co-llvm-$LLVM_RELEASE-nm.patch

  # see llvm/tools/llvm-cvtres/CMakeLists.txt
  TD_SRC="$LLVMSRC/tools/llvm-nm/Opts.td"
  echo "llvm-tblgen $(_relpath "$TD_SRC") -> src/llvm/llvm-nm-opts.inc"
  "$LLVMBOX"/bin/llvm-tblgen \
    -no-warn-on-unused-template-args --write-if-changed --gen-opt-parser-defs \
    -I "$LLVMSRC/tools/llvm-nm" -I "$LLVMSRC/include" \
    "$TD_SRC" -o src/llvm/llvm-nm-opts.inc

  echo "$SRC_VERSION_LINE" >> "$DSTFILE"
  _popd
fi

# —————————————————————————————————————————————————————————————————————————————————
# generate lib/sysinc if missing

if _need_regenerate_sysinc_dir; then
  _regenerate_sysinc_dir
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
      # kill -- -$RUN_PID 2>/dev/null || echo "killing #$RUN_PID"
      echo "killing #$RUN_PID"
      kill $RUN_PID 2>/dev/null || true
      pkill -15 -P $RUN_PID 2>/dev/null || true
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
    "$BASH" "./$(basename "$0")" ${NON_WATCH_ARGS[@]:-} "$@" || BUILD_OK=
    printf "\e[2m> watching files for changes...\e[m\n"
    if [ -n "$BUILD_OK" -a -n "$RUN" ]; then
      export ASAN_OPTIONS=detect_stack_use_after_return=1
      export UBSAN_OPTIONS=print_stacktrace=1
      _killcmd
      ( "$BASH" -c "trap \"pkill -P \$\$\" SIGTERM SIGINT SIGHUP; $RUN" &
        RUN_PID=$!
        echo $RUN_PID > "$RUN_PIDFILE"
        echo "$RUN (#$RUN_PID) started"
        set +e
        wait $RUN_PID
        echo "$RUN [$RUN_PID] exited: $?"
        rm -f "$RUN_PIDFILE"
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
  -feliminate-unused-debug-types \
  -fvisibility=hidden \
  -fcolor-diagnostics \
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

# version
CO_VERSION=$(cat "$PROJECT/version.txt")
IFS=. read -r CO_VER_MAJ CO_VER_MIN CO_VER_BUILD <<< "$CO_VERSION"
IFS=+ read -r CO_VER_BUILD CO_VER_TAG <<< "$CO_VER_BUILD"
XFLAGS+=(
  -DCO_VERSION_STR="\\\"$CO_VERSION\\\"" \
  -DCO_VERSION="$(printf "0x%02x%02x%02x00" $CO_VER_MAJ $CO_VER_MIN $CO_VER_BUILD)" \
)
# note: CO_VERSION_GIT is not added to XFLAGS since it would invalidate PCHs,
# and thus the build, whenever git commits are made.
# Instead this is set only for src/main.c
CO_VERSION_GIT=
[ -d "$PROJECT/.git" ] &&
  CO_VERSION_GIT=$(git -C "$PROJECT" rev-parse HEAD)

# arch-and-system-specific flags
case "$HOST_ARCH-$HOST_SYS" in
  x86_64-macos)  LDFLAGS_HOST+=( -Wl,-platform_version,macos,10.15,10.15 ) ;;
  aarch64-macos) LDFLAGS_HOST+=( -Wl,-platform_version,macos,11.0,11.0 ) ;;
esac

# system-specific flags
case "$HOST_SYS" in
  linux) LDFLAGS_HOST+=( -static ) ;;
  macos) LDFLAGS_HOST+=( -Wl,-adhoc_codesign ) ;;
esac

# build-mode-specific flags
case "$BUILD_MODE" in
  debug)
    XFLAGS+=( -g -O0 -DDEBUG -ferror-limit=6 )
    XFLAGS_HOST+=(
      -fno-omit-frame-pointer \
      -fno-optimize-sibling-calls \
      -fmacro-backtrace-limit=0 \
    )
    # Enable sanitizers in debug builds
    # See https://clang.llvm.org/docs/AddressSanitizer.html
    # See https://clang.llvm.org/docs/UndefinedBehaviorSanitizer.html
    #
    # TODO FIXME: sanitizers disabled on linux.
    #
    # Currently llvmbox is static only, without a dylib interpreter
    # (musl is only built statically.)
    # Further, if we link with just -static-libsan, lld still creates a dynamic
    # exe, and if we add -static-pie then the executable crashes, likely because
    # use of incorrect crt start files:
    #   XFLAGS_HOST+=( -static-libsan -fsanitize=undefined )
    #   LDFLAGS_HOST+=( -static-pie -static-libsan -fsanitize=undefined )
    #
    if [ "$HOST_SYS" != linux ]; then
      XFLAGS_HOST+=(
        -fsanitize=address,undefined \
        -fsanitize-address-use-after-scope \
      )
      # ubsan options
      XFLAGS_HOST+=( -fsanitize=signed-integer-overflow,float-divide-by-zero,null,alignment,nonnull-attribute,nullability )
      LDFLAGS_HOST+=( -fsanitize=address,undefined )
    fi
    ;;
  opt*)
    XFLAGS+=( -DNDEBUG )
    XFLAGS_WASM+=( -Oz )
    XFLAGS_HOST+=( -O3 )
    if $DEBUGGABLE; then
      XFLAGS_HOST+=( -g -fno-omit-frame-pointer -fno-optimize-sibling-calls )
    else
      XFLAGS_HOST+=( -fomit-frame-pointer )
    fi
    case $HOST_ARCH in
      # minimum Haswell-gen x86 CPUs <https://clang.llvm.org/docs/UsersManual.html#x86>
      x86_64) XFLAGS_HOST+=( -march=x86-64-v3 ) ;;
    esac
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
        if [ -n "${CO_VERSION_GIT:-}" ] && [ "$srcfile" = src/main.c ]; then
          echo "  FLAGS = -DCO_VERSION_GIT=\"$CO_VERSION_GIT\"" >> $NF
        fi
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

cat << END > config-xflags.tmp
XFLAGS ${XFLAGS[@]:-}
END
cat << END > config.tmp
TARGET $HOST_ARCH-$HOST_SYS
LLVM $LLVMBOX_RELEASE
XFLAGS $(sed -E -e 's/-DCO_VERSION[_A-Za-z0-9]*=[^ ]+ ?//g' <<< "${XFLAGS[@]:-}")
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
END
cat << END > lconfig.tmp
LDFLAGS ${LDFLAGS[@]:-}
LDFLAGS_HOST ${LDFLAGS_HOST[@]:-}
LDFLAGS_WASM ${LDFLAGS_WASM[@]:-}
END
if ! diff -q config config.tmp >/dev/null 2>&1; then
  if [ -e config ]; then
    echo "———————————————————————————————————————————————————"
    echo "build configuration changed:"
    diff -wu -U 0 -L cached config -L current config.tmp || true
    echo "———————————————————————————————————————————————————"
  fi
  mv config.tmp config
  rm -rf "$OUT_DIR/obj" "$OUT_DIR/lto-cache"
else
  if ! diff -q config-xflags config-xflags.tmp >/dev/null 2>&1; then
    # Note: this usually happens when CO_VERSION changes
    [ -e config-xflags ] && echo "xflags changed; wiping PCHs"
    # if [ -e config-xflags ]; then
    #   echo "———————————————————————————————————————————————————"
    #   echo "xflags changed; wiping PCHs:"
    #   diff -wu -U 0 -L cached config-xflags -L current config-xflags.tmp || true
    #   echo "———————————————————————————————————————————————————"
    # fi
    find "$OUT_DIR" -type f -name '*.pch' -delete
    mv config-xflags.tmp config-xflags
  fi
  if ! diff -q lconfig lconfig.tmp >/dev/null 2>&1; then
    [ -e lconfig ] && echo "linker configuration changed"
    mv lconfig.tmp lconfig
    rm -f "$OUT_DIR/$MAIN_EXE"
  fi
fi
rm -f config-xflags.tmp config.tmp lconfig.tmp

# —————————————————————————————————————————————————————————————————————————————————
# run ninja

cd "$PROJECT"
if [ -n "$RUN" ]; then
  "$NINJA" "${NINJA_ARGS[@]}" "$@"
  echo $RUN
  exec "${BASH:-bash}" -c "$RUN"
fi
exec "$NINJA" "${NINJA_ARGS[@]}" "$@"
