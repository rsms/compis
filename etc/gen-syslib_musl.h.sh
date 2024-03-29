#!/usr/bin/env bash
set -euo pipefail
source "$(dirname "$0")/lib.sh"

HEADER_FILE="$PROJECT/src/syslib_musl.h"
SOURCE_DESTDIR="$PROJECT/lib/musl"

_pushd "$SOURCE_DESTDIR"
echo "generating $(_relpath "$HEADER_FILE")"

cat << END > "$HEADER_FILE"
// Do not edit! Generated by ${SCRIPT_FILE##$PROJECT/}
// SPDX-License-Identifier: Apache-2.0
END

# —————————————————————————————————————————————————————————————————————————————————————
# generate source list

echo "static const char* const musl_sources[] = {" >> "$HEADER_FILE"
n=0

_gen_sources() {
  local f fn var
  for f in "$@"; do
    echo "  \"$f\"," >> "$HEADER_FILE"
    # record source index for file
    var=${f//\//___} # foo/bar/b-z.c => foo___bar___b-z.c
    var=${var//-/__} # foo___bar___b-z.c => foo___bar___b__z.c
    var=${var//./_}  # foo___bar___b__z.c => foo___bar___b__z_c
    declare -g SRCIDX__${var}=$n
    (( n += 1 ))
  done
}

GENERIC_SOURCES=( src/*/*.c src/malloc/mallocng/*.c )
_gen_sources "${GENERIC_SOURCES[@]}"

TIME32_SOURCES=( compat/time32/*.c )
_gen_sources "${TIME32_SOURCES[@]}"

# ARCH_{arch}_SOURCES=()
for target_info in $(_co_targets); do  # ( "arch,sys,sysver,..." ... )
  IFS=, read -r arch sys sysver ign <<< "$target_info"
  [ "$sys" = linux ] || continue
  ARCH_SOURCES=()
  for f in src/*/$arch/*.[csS] src/malloc/mallocng/$arch/*.[csS]; do
    [ -f "$f" ] || continue
    ARCH_SOURCES+=( $f )
    # record presence of implementation, for shadowing generic implementation
    name=${f%.*}               # src/x-y.k/aarch64/round.c => src/x-y.k/aarch64/round
    name=${name//\/$arch\//\/} # src/x-y.k/aarch64/round => src/x-y.k/round
    name=${name//\//___}       # src/x-y.k/round => src___x-y.k___round
    name=${name//-/__}         # src___x-y.k___round => src___x__y.k___round
    name=${name//./_}          # src___x__y.k___round => src___x__y_k___round
    declare ARCH_${arch}_HAS_${name}=1
  done
  _gen_sources "${ARCH_SOURCES[@]}"
  ARCH_SOURCES="${ARCH_SOURCES[@]}"
  declare ARCH_${arch}_SOURCES="$ARCH_SOURCES"
done

BM_NBYTE=$(( ($n + 7) / 8 ))  # number of bytes needed for n bits
echo "};" >> "$HEADER_FILE"
echo "" >> "$HEADER_FILE"

# —————————————————————————————————————————————————————————————————————————————————————

# compile helper program
"$LLVMBOX"/bin/clang -std=c11 "$PROJECT"/etc/bitset.c -o "$PROJECT"/etc/bitset

_target_sources() {
  local indices=()
  local f name extra_sources

  # generic sources
  for f in ${GENERIC_SOURCES[@]}; do
    [ -f "$f" ] || continue

    # if there's an arch-specific implementation, skip the generic one
    name=${f%.*}         # src/x-y.k/aarch64/round.c => src/x-y.k/aarch64/round
    name=${name//\//___} # src/x-y.k/round => src___x-y.k___round
    name=${name//-/__}   # src___x-y.k___round => src___x__y.k___round
    name=${name//./_}    # src___x__y.k___round => src___x__y_k___round
    name=ARCH_${arch}_HAS_${name}
    #[ -z "${!name:-}" ] || echo "skip ($arch) $f"
    [ -z "${!name:-}" ] || continue

    # lookup source index for file
    name=${f//\//___}
    name=${name//-/__}
    name=${name//./_}
    name=SRCIDX__${name}
    indices+=( ${!name} )
  done

  # extra sources, depending on arch
  extra_sources=()
  case "$arch" in
    i386|arm) extra_sources+=( ${TIME32_SOURCES[@]} )
  esac

  # arch-specific sources
  name=ARCH_${arch}_SOURCES
  for f in ${!name} ${extra_sources:-}; do
    # lookup source index for file
    name=${f//\//___}
    name=${name//-/__}
    name=${name//./_}
    name=SRCIDX__${name}
    indices+=( ${!name} )
  done

  "$PROJECT"/etc/bitset -n "${indices[@]}"
  # echo "  ${indices[@]}"
}

_crt_source() {
  local f=$arch/$1.s
  [ -f "crt/$f" ] || f=$arch/$1.S
  [ -f "crt/$f" ] || f=$1.c
  [ -f "crt/$f" ] || _err "crt source $1 not found ($arch)"
  printf "\"$f\","
}

(
  echo "typedef struct {"
  echo "  targetdesc_t target;"
  echo ""
  echo "  // crt sources, based in dir \"crt/\""
  echo "  const char* crt1;"
  echo "  const char* rcrt1;"
  echo "  const char* Scrt1;"
  echo "  const char* crti;"
  echo "  const char* crtn;"
  echo ""
  echo "  // bitmap index into musl_sources"
  echo "  u8 sources[$BM_NBYTE];"
  echo "} musl_srclist_t;"
  echo "static const musl_srclist_t musl_srclist[] = {"
  for target_info in $(_co_targets); do  # ( "arch,sys,sysver,..." ... )
    IFS=, read -r arch sys sysver ign <<< "$target_info"
    [ "$sys" = linux ] || continue
    printf "  {{ARCH_%s, SYS_%s, \"%s\"}, " $arch $sys $sysver
    _crt_source crt1
    _crt_source rcrt1
    _crt_source Scrt1
    _crt_source crti
    _crt_source crtn
    printf " {\n   "
    _target_sources
    echo "}},"
  done
  echo "};"
) >> "$HEADER_FILE"
