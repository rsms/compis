#!/bin/bash
set -e
ORIG_PWD=$PWD
PROG=$0
cd "$(dirname "$0")"
_err() { echo "$PROG:" "$@" >&2 ; exit 1; }
_checksum() { sha256sum "$@" | cut -d' ' -f1; }

# project constants
SRCDIR=src
MAIN_EXE=co
PP_PREFIX=CO_
WASM_SYMS=etc/wasm.syms

# variables overrideable via environment variables
DEPSDIR=${DEPSDIR:-$PWD/deps}
NINJA=${NINJA:-ninja}
XFLAGS=( $XFLAGS )
CFLAGS=( $CFLAGS -fms-extensions -Wno-microsoft )
CXXFLAGS=()

# variables configurable via CLI flags
OUTDIR=
OUTDIR_DEFAULT=out
BUILD_MODE=opt  # opt | opt-fast | debug
WATCH=
WATCH_ADDL_FILES=()
_WATCHED=
WITH_LLVM=
RUN=
NINJA_ARGS=()
NON_WATCH_ARGS=()
TESTING_ENABLED=false
ONLY_CONFIGURE=false
STRIP=false
STATIC=false
DEBUGGABLE=false
VERBOSE=false

while [[ $# -gt 0 ]]; do
  case "$1" in
    -w)      WATCH=1; shift; continue ;;
    -_w_)    _WATCHED=1; shift; continue ;;
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
  -static)   STATIC=true; shift ;;
  -out=*)    OUTDIR=${1:5}; shift; continue ;;
  -g)        DEBUGGABLE=true; shift ;;
  -v)        VERBOSE=true; NINJA_ARGS+=(-v); shift ;;
  -D*)       [ ${#1} -gt 2 ] || _err "Missing NAME after -D";XFLAGS+=( "$1" );shift;;
  -llvm=*)   WITH_LLVM=${1:6}; shift ;;
  -h|-help|--help) cat << _END
usage: $0 [options] [--] [<target> ...]
Build mode option: (select just one)
  -opt           Build optimized product with some assertions enabled (default)
  -opt-fast      Build optimized product without any assertions
  -debug         Build debug product with assertions and tracing capabilities
  -config        Just configure; generate ninja files (don't actually build)
Output options:
  -g             Make -opt build extra debuggable (basic opt only, frame pointers)
  -strip         Do not include debug data (negates -g)
  -out=<dir>     Build in <dir> instead of "$OUTDIR_DEFAULT/<mode>".
  -DNAME[=value] Define CPP variable NAME with value
  -llvm=<how>    Link llvm libs "static" or "shared"
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

#————————————————————————————————————————————————————————————————————————————————————————

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
    bash "./$(basename "$0")" -_w_ "${NON_WATCH_ARGS[@]}" "$@" || BUILD_OK=
    printf "\e[2m> watching files for changes...\e[m\n"
    if [ -n "$BUILD_OK" -a -n "$RUN" ]; then
      export ASAN_OPTIONS=detect_stack_use_after_return=1
      export UBSAN_OPTIONS=print_stacktrace=1
      _killcmd
      ( $SHELL -c "$RUN" &
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
    _fswatch "$SRCDIR" "$(basename "$0")" "${WATCH_ADDL_FILES[@]}"
  done
  exit 0
fi

#————————————————————————————————————————————————————————————————————————————————————————
# setup environment (select compiler, clear $OUTDIR if compiler changed)

# set DEBUG based on BUILD_MODE
DEBUG=true; [ "$BUILD_MODE" != "debug" ] && DEBUG=false

# set OUTDIR, unless set with -out=<dir>
[ -z "$OUTDIR" ] && OUTDIR=$OUTDIR_DEFAULT/$BUILD_MODE

# set WITH_LLVM
case "$WITH_LLVM" in
  "")            WITH_LLVM=static ; $DEBUG && WITH_LLVM=shared ;;
  static|shared) ;;
  off)           WITH_LLVM= ;;
  *)             _err "invalid value \"$WITH_LLVM\" for -llvm option" ;;
esac

export PATH=$DEPSDIR/llvm/bin:$PATH
export CC=clang
export CXX=clang++
CC_FILE=$DEPSDIR/llvm/bin/clang
CC_IS_CLANG=true
CC_IS_GCC=false

$VERBOSE && { echo "CC=$CC"; echo "CXX=$CXX"; echo "CC_FILE=$CC_FILE"; }

# check compiler and clear $OUTDIR if compiler changed.
# Note that ninja takes care of rebuilding if flags changed.
[ -x "$CC_FILE" ] || _err "LLVM not built. Run ./init.sh"
CC_STAMP_FILE=$OUTDIR/cc.stamp
if [ "$CC_FILE" -nt "$CC_STAMP_FILE" -o "$CC_FILE" -ot "$CC_STAMP_FILE" ]; then
  [ -f "$CC_STAMP_FILE" ] && echo "$CC_FILE changed; clearing OUTDIR"
  rm -rf "$OUTDIR"
  mkdir -p "$OUTDIR"
  touch -r "$CC_FILE" "$CC_STAMP_FILE"
fi

#————————————————————————————————————————————————————————————————————————————————————————
# construct flags
#
#   XFLAGS             compiler flags (common to C and C++)
#     XFLAGS_HOST      compiler flags specific to native host target
#     XFLAGS_WASM      compiler flags specific to WASM target
#     CFLAGS           compiler flags for C
#       CFLAGS_HOST    compiler flags for C specific to native host target
#       CFLAGS_WASM    compiler flags for C specific to WASM target
#     CXXFLAGS         compiler flags for C++
#       CXXFLAGS_HOST  compiler flags for C++ specific to native host target
#       CXXFLAGS_WASM  compiler flags for C++ specific to WASM target
#   LDFLAGS            linker flags common to all targets
#     LDFLAGS_HOST     linker flags specific to native host target (cc suite's ld)
#     LDFLAGS_WASM     linker flags specific to WASM target (llvm's wasm-ld)
#————————————————————————————
XFLAGS=(
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
  "${XFLAGS[@]}" \
)
XFLAGS_HOST=()
XFLAGS_WASM=(
  -D${PP_PREFIX}NO_LIBC \
  --target=wasm32 \
  --no-standard-libraries \
  -fvisibility=hidden \
)
#————————————————————————————
CFLAGS=(
  -std=c11 \
  "${CFLAGS[@]}" \
)
CFLAGS_HOST=()
CFLAGS_WASM=()
#————————————————————————————
CXXFLAGS=(
  -std=c++14 \
  -fvisibility-inlines-hidden \
  -fno-exceptions \
  -fno-rtti \
  "${CXXFLAGS[@]}" \
)
CXXFLAGS_HOST=()
CXXFLAGS_WASM=()
#————————————————————————————
LDFLAGS_HOST=(
  $LDFLAGS \
)
LDFLAGS_WASM=(
  --no-entry \
  --no-gc-sections \
  --export-dynamic \
  --import-memory \
  $LDFLAGS_WASM \
)
#————————————————————————————
# compiler-specific flags
if $CC_IS_CLANG; then
  XFLAGS+=(
    -Wcovered-switch-default \
    -Werror=format-insufficient-args \
    -Werror=bitfield-constant-conversion \
    -Wno-pragma-once-outside-header \
  )
  [ -t 1 ] && XFLAGS+=( -fcolor-diagnostics )

  if [ "$(uname -s)" = "Darwin" ] &&
     [ "$WITH_LLVM" != "shared" -o "$(uname -m)" != "arm64" ]
  then
    # Use lld to avoid incompatible outdated macOS system linker.
    # If the system linker is outdated, it would fail with an error like this:
    # "ld: could not parse object file ... Unknown attribute kind ..."
    #
    # Note on second check in "if" above:
    #   macOS 11 introduced a complex dynamic linker which lld struggles with.
    #   From Apple: (62986286)
    #     New in macOS Big Sur 11.0.1, the system ships with a built-in dynamic
    #     linker cache of all system-provided libraries. As part of this
    #     change, copies of dynamic libraries are no longer present on the
    #     filesystem. Code that attempts to check for dynamic library presence
    #     by looking for a file at a path or enumerating a directory will fail.
    #     Instead, check for library presence by attempting to dlopen() the
    #     path, which will correctly check for the library in the cache.
    #     <https://developer.apple.com/documentation/macos-release-notes/
    #      macos-big-sur-11_0_1-release-notes#Kernel>
    #   If we try to link using lld on macOS 12.2.1 with our llvm-build of
    #   lld 14.0.0, we get the following error:
    #     error: LC_DYLD_INFO_ONLY not found in deps/llvm/lib/libco-llvm-bundle-d.dylib
    #
    MACOS_VERSION=10.15
    [ "$(uname -m)" = "arm64" ] && MACOS_VERSION=12.0
    LDFLAGS_HOST+=(
      -fuse-ld="$(dirname "$CC_FILE")/ld64.lld" \
      -Wl,-platform_version,macos,$MACOS_VERSION,$MACOS_VERSION \
    )
  fi
elif $CC_IS_GCC; then
  [ -t 1 ] && XFLAGS+=( -fdiagnostics-color=always )
fi

# build mode- and compiler-specific flags
if $DEBUG; then
  XFLAGS+=( -O0 -DDEBUG )
  if $CC_IS_CLANG; then
    XFLAGS+=( -ferror-limit=6 )
    # enable llvm address and UD sanitizer in debug builds
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
  fi
else
  XFLAGS+=( -DNDEBUG )
  if $DEBUGGABLE; then
    XFLAGS_HOST+=( -O1 -mtune=native \
      -fno-omit-frame-pointer \
      -fno-optimize-sibling-calls \
    )
  else
    XFLAGS_HOST+=( -O3 -mtune=native -fomit-frame-pointer )
  fi
  XFLAGS_WASM+=( -Oz )
  LDFLAGS_WASM+=( --lto-O3 --no-lto-legacy-pass-manager )
  # LDFLAGS_WASM+=( -z stack-size=$[128 * 1024] ) # larger stack, smaller heap
  # LDFLAGS_WASM+=( --compress-relocations --strip-debug )
  # LDFLAGS_HOST+=( -dead_strip )
  if [ "$BUILD_MODE" = "opt" ]; then
    XFLAGS+=( -D${PP_PREFIX}SAFE )
  fi

  # enable LTO
  if $CC_IS_CLANG; then
    # XFLAGS+=( -flto )
    # LDFLAGS_HOST+=( -flto )
    XFLAGS+=( -flto=thin )
    LDFLAGS_HOST+=(
      -flto=thin \
      -Wl,--lto-O3 \
      -Wl,-prune_after_lto,86400 \
      -Wl,-cache_path_lto,"'$OUTDIR/lto-cache'" \
      -Wl,-object_path_lto,"'$OUTDIR/lto-obj'" \
    )
  fi
fi

# testing enabled?
$TESTING_ENABLED &&
  XFLAGS+=( -D${PP_PREFIX}TESTING_ENABLED )

# llvm?
# LLVM_CFLAGS & LLVM_CXXFLAGS are only used for source files in src/llvm/
LLVM_CFLAGS=()
LLVM_CXXFLAGS=()
if [ -n "$WITH_LLVM" ]; then
  XFLAGS+=( -DWITH_LLVM )
  CFLAGS_HOST+=( -I$DEPSDIR/llvm/include )
  CXXFLAGS_HOST+=( -I$DEPSDIR/llvm/include )
  LLVM_CFLAGS=( $($DEPSDIR/llvm/bin/llvm-config --cflags) )
  LLVM_CXXFLAGS=(
    -nostdinc++ -I$DEPSDIR/llvm/include/c++/v1 -stdlib=libc++ \
    $($DEPSDIR/llvm/bin/llvm-config --cxxflags) \
  )
  LDFLAGS_HOST+=(
    -lm \
  )
  if [ "$WITH_LLVM" = shared ]; then
    LDFLAGS_HOST+=(
      "-Wl,-rpath,$DEPSDIR/llvm/lib" \
      "$DEPSDIR/llvm/lib/libco-llvm-bundle-d.dylib" \
    )
  else
    LDFLAGS_HOST+=(
      "$DEPSDIR/llvm/lib/libco-llvm-bundle.a" \
      "$DEPSDIR/llvm/lib/libc++.a" \
      "$DEPSDIR/llvm/lib/libunwind.a" \
    )
  fi
fi

#————————————————————————————————————————————————————————————————————————————————————————
# find source files
#
# name.{c,cc}       always included
# name.ARCH.{c,cc}  specific to ARCH (e.g. wasm, x86_64, arm64, etc)
# name.test.{c,cc}  only included when testing is enabled

COMMON_SOURCES=()
HOST_SOURCES=()
WASM_SOURCES=()
TEST_SOURCES=()

HOST_ARCH=$(uname -m)
for f in $(find "$SRCDIR" -name '*.c' -or -name '*.cc'); do
  case "$f" in
    */test.c|*.test.c|*.test.cc)    TEST_SOURCES+=( "$f" ) ;;
    *.$HOST_ARCH.c|*.$HOST_ARCH.cc) HOST_SOURCES+=( "$f" ) ;;
    *.wasm.c|*.wasm.cc)             WASM_SOURCES+=( "$f" ) ;;
    *)                              COMMON_SOURCES+=( "$f" ) ;;
  esac
done

$VERBOSE && {
  echo "TEST_SOURCES=${TEST_SOURCES[@]}"
  echo "HOST_SOURCES=${HOST_SOURCES[@]}"
  echo "WASM_SOURCES=${WASM_SOURCES[@]}"
  echo "COMMON_SOURCES=${COMMON_SOURCES[@]}"
}

#————————————————————————————————————————————————————————————————————————————————————————
# generate .clang_complete

if $CC_IS_CLANG; then
  echo "-I$(realpath "$SRCDIR")" > .clang_complete
  for flag in "${XFLAGS[@]}" "${XFLAGS_HOST[@]}" "${CFLAGS_HOST[@]}"; do
    echo "$flag" >> .clang_complete
  done
fi

#————————————————————————————————————————————————————————————————————————————————————————
# generate build.ninja

NF=$OUTDIR/new-build.ninja     # temporary file
NINJAFILE=$OUTDIR/build.ninja  # actual build file
NINJA_ARGS+=( -f "$NINJAFILE" )
LINKER=$CC

mkdir -p "$OUTDIR/obj"

cat << _END > $NF
ninja_required_version = 1.3
builddir = $OUTDIR
objdir = \$builddir/obj

xflags = ${XFLAGS[@]}
xflags_host = \$xflags ${XFLAGS_HOST[@]}
xflags_wasm = \$xflags ${XFLAGS_WASM[@]}

cflags = ${CFLAGS[@]}
cflags_host = \$xflags_host ${CFLAGS_HOST[@]}
cflags_wasm = \$xflags_wasm ${CFLAGS_WASM[@]}

cxxflags = ${CXXFLAGS[@]}
cxxflags_host = \$xflags_host ${CXXFLAGS_HOST[@]}
cxxflags_wasm = \$xflags_wasm ${CXXFLAGS_WASM[@]}

ldflags_host = ${LDFLAGS_HOST[@]}
ldflags_wasm = ${LDFLAGS_WASM[@]}


rule link
  command = $LINKER \$ldflags_host \$FLAGS -o \$out \$in
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
  generator = true

build src/parse/ast_gen.h src/parse/ast_gen.c: ast_gen src/parse/ast.h | src/parse/ast_gen.py
build src/parse/parser_gen.h: parse_gen src/parse/parser.c | src/parse/parse_gen.py

_END

if [ -n "$WITH_LLVM" ]; then
  echo "build \$objdir/llvm-includes.pch: cxx_pch_gen src/llvm/llvm-includes.hh" >> $NF
  echo "  FLAGS = ${LLVM_CXXFLAGS[@]}" >> $NF
fi


_objfile() { echo \$objdir/${1//\//.}.o; }
_gen_obj_build_rules() {
  local TARGET=$1 ; shift
  local OBJECT
  local CC_RULE=cc
  local CXX_RULE=cxx
  if [ "$TARGET" = wasm ]; then
    CC_RULE=cc_wasm
    CXX_RULE=cxx_wasm
  fi
  for SOURCE in "$@"; do
    OBJECT=$(_objfile "$TARGET-$SOURCE")
    case "$SOURCE" in
      */llvm/*.c)
        [ -z "$WITH_LLVM" ] && continue
        echo "build $OBJECT: $CC_RULE $SOURCE" >> $NF
        echo "  FLAGS = ${LLVM_CFLAGS[@]}" >> $NF
        ;;
      */llvm/*.cc)
        [ -z "$WITH_LLVM" ] && continue
        echo "build $OBJECT: $CXX_RULE $SOURCE | \$objdir/llvm-includes.pch" >> $NF
        echo "  FLAGS = -include-pch \$objdir/llvm-includes.pch ${LLVM_CXXFLAGS[@]}" >> $NF
        ;;
      *.c)
        echo "build $OBJECT: $CC_RULE $SOURCE" >> $NF
        ;;
      *.cc)
        echo "build $OBJECT: $CXX_RULE $SOURCE" >> $NF
        ;;
      *) _err "don't know how to compile this file type ($SOURCE)"
    esac
    echo "$OBJECT"
  done
}

HOST_SOURCES+=( "${COMMON_SOURCES[@]}" )
WASM_SOURCES+=( "${COMMON_SOURCES[@]}" )

if $TESTING_ENABLED; then
  HOST_SOURCES+=( "${TEST_SOURCES[@]}" )
  WASM_SOURCES+=( "${TEST_SOURCES[@]}" )
fi

HOST_OBJECTS=( $(_gen_obj_build_rules "host" "${HOST_SOURCES[@]}") )
echo >> $NF
echo "build \$builddir/$MAIN_EXE: link ${HOST_OBJECTS[@]}" >> $NF
echo >> $NF

WASM_OBJECTS=( $(_gen_obj_build_rules "wasm" "${WASM_SOURCES[@]}") )
echo >> $NF
echo "build \$builddir/$MAIN_EXE.wasm: link_wasm ${WASM_OBJECTS[@]}" >> $NF
echo >> $NF

echo "build $MAIN_EXE: phony \$builddir/$MAIN_EXE" >> $NF
echo "build $MAIN_EXE.wasm: phony \$builddir/$MAIN_EXE.wasm" >> $NF
echo "default $MAIN_EXE" >> $NF

# write build.ninja only if it changed
if [ "$(_checksum $NF)" != "$(_checksum "$NINJAFILE" 2>/dev/null)" ]; then
  mv $NF "$NINJAFILE"
  $VERBOSE && echo "wrote $NINJAFILE"
else
  rm $NF
  $VERBOSE && echo "$NINJAFILE is up to date"
fi

# stop now if -config is set
$ONLY_CONFIGURE && exit

# ninja
if [ -n "$RUN" ]; then
  $NINJA "${NINJA_ARGS[@]}" "$@"
  echo $RUN
  exec $SHELL -c "$RUN"
fi
exec $NINJA "${NINJA_ARGS[@]}" "$@"
