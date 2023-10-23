#!/usr/bin/env bash
set -euo pipefail
PWD0=$PWD
SCRIPTNAME=$(realpath "$0")
cd "$(dirname "$0")/.."
source etc/lib.sh
PROJECT=$PWD
SCRIPTNAME=${SCRIPTNAME:$(( ${#PROJECT} + 1 ))}

DESTDIR="$PROJECT/lib/librt"
HEADER_FILE="$PROJECT/src/syslib_librt.h"

# ————————————————————————————————————————————————————————————————————————————————————
# get source

eval `awk '$1 ~ /^LLVM_RELEASE=/' build.sh`

SRCDIR="$PROJECT/deps/download/compiler-rt-$LLVM_RELEASE"

if [ ! -d "$SRCDIR/lib/builtins" ]; then
  ARCHIVE=compiler-rt-$LLVM_RELEASE.src.tar.xz
  rm -rf "$SRCDIR"
  mkdir -p "$SRCDIR"
  _pushd "$SRCDIR"
  if [ ! -f ../$ARCHIVE ]; then
    wget -O ../$ARCHIVE \
    https://github.com/llvm/llvm-project/releases/download/llvmorg-$LLVM_RELEASE/$ARCHIVE
  fi
  echo "extracting $ARCHIVE"
  XZ_OPT='-T0' tar --strip-components 1 -xf ../$ARCHIVE
  _popd
fi

# ————————————————————————————————————————————————————————————————————————————————————
# find & copy sources

_pushd "$SRCDIR/lib/builtins"

_deps() { for f in "$1"/*.{h,inc}; do [ ! -f "$f" ] || echo $f; done; }
#
# when upgrading compiler-rt to a new version, perform these manual steps:
# - open lib/builtins/CMakeLists.txt
# - update SOURCES arrays below with GENERIC_SOURCES + GENERIC_TF_SOURCES
#
GENERIC_SOURCES=( # cmake: GENERIC_SOURCES + GENERIC_TF_SOURCES
  absvdi2.c \
  absvsi2.c \
  absvti2.c \
  adddf3.c \
  addsf3.c \
  addvdi3.c \
  addvsi3.c \
  addvti3.c \
  apple_versioning.c \
  ashldi3.c \
  ashlti3.c \
  ashrdi3.c \
  ashrti3.c \
  bswapdi2.c \
  bswapsi2.c \
  clzdi2.c \
  clzsi2.c \
  clzti2.c \
  cmpdi2.c \
  cmpti2.c \
  comparedf2.c \
  comparesf2.c \
  ctzdi2.c \
  ctzsi2.c \
  ctzti2.c \
  divdc3.c \
  divdf3.c \
  divdi3.c \
  divmoddi4.c \
  divmodsi4.c \
  divmodti4.c \
  divsc3.c \
  divsf3.c \
  divsi3.c \
  divti3.c \
  extendsfdf2.c \
  extendhfsf2.c \
  ffsdi2.c \
  ffssi2.c \
  ffsti2.c \
  fixdfdi.c \
  fixdfsi.c \
  fixdfti.c \
  fixsfdi.c \
  fixsfsi.c \
  fixsfti.c \
  fixunsdfdi.c \
  fixunsdfsi.c \
  fixunsdfti.c \
  fixunssfdi.c \
  fixunssfsi.c \
  fixunssfti.c \
  floatdidf.c \
  floatdisf.c \
  floatsidf.c \
  floatsisf.c \
  floattidf.c \
  floattisf.c \
  floatundidf.c \
  floatundisf.c \
  floatunsidf.c \
  floatunsisf.c \
  floatuntidf.c \
  floatuntisf.c \
  fp_mode.c \
  int_util.c \
  lshrdi3.c \
  lshrti3.c \
  moddi3.c \
  modsi3.c \
  modti3.c \
  muldc3.c \
  muldf3.c \
  muldi3.c \
  mulodi4.c \
  mulosi4.c \
  muloti4.c \
  mulsc3.c \
  mulsf3.c \
  multi3.c \
  mulvdi3.c \
  mulvsi3.c \
  mulvti3.c \
  negdf2.c \
  negdi2.c \
  negsf2.c \
  negti2.c \
  negvdi2.c \
  negvsi2.c \
  negvti2.c \
  os_version_check.c \
  paritydi2.c \
  paritysi2.c \
  parityti2.c \
  popcountdi2.c \
  popcountsi2.c \
  popcountti2.c \
  powidf2.c \
  powisf2.c \
  subdf3.c \
  subsf3.c \
  subvdi3.c \
  subvsi3.c \
  subvti3.c \
  trampoline_setup.c \
  truncdfhf2.c \
  truncdfsf2.c \
  truncsfhf2.c \
  ucmpdi2.c \
  ucmpti2.c \
  udivdi3.c \
  udivmoddi4.c \
  udivmodsi4.c \
  udivmodti4.c \
  udivsi3.c \
  udivti3.c \
  umoddi3.c \
  umodsi3.c \
  umodti3.c \
  \
  addtf3.c \
  comparetf2.c \
  divtc3.c \
  divtf3.c \
  extenddftf2.c \
  extendhftf2.c \
  extendsftf2.c \
  fixtfdi.c \
  fixtfsi.c \
  fixtfti.c \
  fixunstfdi.c \
  fixunstfsi.c \
  fixunstfti.c \
  floatditf.c \
  floatsitf.c \
  floattitf.c \
  floatunditf.c \
  floatunsitf.c \
  floatuntitf.c \
  multc3.c \
  multf3.c \
  powitf2.c \
  subtf3.c \
  trunctfdf2.c \
  trunctfhf2.c \
  trunctfsf2.c \
  \
  clear_cache.c \
)
# TODO:
# - if target has __bf16: GENERIC_SOURCES+=( truncdfbf2.c truncsfbf2.c )
#   e.g. if_compiles($TARGET, "__bf16 f(__bf16 x) { return x; }")
DARWIN_SOURCES=( # cmake: if (APPLE) GENERIC_SOURCES+=...
  atomic_flag_clear.c \
  atomic_flag_clear_explicit.c \
  atomic_flag_test_and_set.c \
  atomic_flag_test_and_set_explicit.c \
  atomic_signal_fence.c \
  atomic_thread_fence.c \
)
X86_ARCH_SOURCES=( # these files are used on 32-bit and 64-bit x86
  cpu_model.c \
  i386/fp_mode.c \
)
# Implement extended-precision builtins, assuming long double is 80 bits.
# long double is not 80 bits on Android or MSVC.
X86_80_BIT_SOURCES=(
  divxc3.c \
  fixxfdi.c \
  fixxfti.c \
  fixunsxfdi.c \
  fixunsxfsi.c \
  fixunsxfti.c \
  floatdixf.c \
  floattixf.c \
  floatundixf.c \
  floatuntixf.c \
  mulxc3.c \
  powixf2.c \
)
X86_64_SOURCES=(
  x86_64/floatdidf.c \
  x86_64/floatdisf.c \
  x86_64/floatundidf.S \
  x86_64/floatundisf.S \
)
X86_64_SOURCES+=( # cmake: if (NOT ANDROID)
  x86_64/floatdixf.c \
  x86_64/floatundixf.S \
  "${X86_80_BIT_SOURCES[@]}" \
)
# X86_64_SOURCES_WIN32=( # cmake: if (WIN32)
#   ${X86_64_SOURCES[@]} \
#   x86_64/chkstk.S \
#   x86_64/chkstk2.S \
# )
I386_SOURCES=(
  i386/ashldi3.S \
  i386/ashrdi3.S \
  i386/divdi3.S \
  i386/floatdidf.S \
  i386/floatdisf.S \
  i386/floatundidf.S \
  i386/floatundisf.S \
  i386/lshrdi3.S \
  i386/moddi3.S \
  i386/muldi3.S \
  i386/udivdi3.S \
  i386/umoddi3.S \
)
I386_SOURCES+=( # cmake: if (NOT ANDROID)
  i386/floatdixf.S \
  i386/floatundixf.S \
  "${X86_80_BIT_SOURCES[@]}" \
)
# I386_SOURCES_WIN32=( # cmake: if (WIN32)
#   ${I386_SOURCES[@]} \
#   i386/chkstk.S \
#   i386/chkstk2.S \
# )
ARM_SOURCES=(
  arm/fp_mode.c \
  arm/bswapdi2.S \
  arm/bswapsi2.S \
  arm/clzdi2.S \
  arm/clzsi2.S \
  arm/comparesf2.S \
  arm/divmodsi4.S \
  arm/divsi3.S \
  arm/modsi3.S \
  arm/sync_fetch_and_add_4.S \
  arm/sync_fetch_and_add_8.S \
  arm/sync_fetch_and_and_4.S \
  arm/sync_fetch_and_and_8.S \
  arm/sync_fetch_and_max_4.S \
  arm/sync_fetch_and_max_8.S \
  arm/sync_fetch_and_min_4.S \
  arm/sync_fetch_and_min_8.S \
  arm/sync_fetch_and_nand_4.S \
  arm/sync_fetch_and_nand_8.S \
  arm/sync_fetch_and_or_4.S \
  arm/sync_fetch_and_or_8.S \
  arm/sync_fetch_and_sub_4.S \
  arm/sync_fetch_and_sub_8.S \
  arm/sync_fetch_and_umax_4.S \
  arm/sync_fetch_and_umax_8.S \
  arm/sync_fetch_and_umin_4.S \
  arm/sync_fetch_and_umin_8.S \
  arm/sync_fetch_and_xor_4.S \
  arm/sync_fetch_and_xor_8.S \
  arm/udivmodsi4.S \
  arm/udivsi3.S \
  arm/umodsi3.S \
)
# thumb1_SOURCES is ignored (<armv7)
ARM_EABI_SOURCES=(
  arm/aeabi_cdcmp.S \
  arm/aeabi_cdcmpeq_check_nan.c \
  arm/aeabi_cfcmp.S \
  arm/aeabi_cfcmpeq_check_nan.c \
  arm/aeabi_dcmp.S \
  arm/aeabi_div0.c \
  arm/aeabi_drsub.c \
  arm/aeabi_fcmp.S \
  arm/aeabi_frsub.c \
  arm/aeabi_idivmod.S \
  arm/aeabi_ldivmod.S \
  arm/aeabi_memcmp.S \
  arm/aeabi_memcpy.S \
  arm/aeabi_memmove.S \
  arm/aeabi_memset.S \
  arm/aeabi_uidivmod.S \
  arm/aeabi_uldivmod.S \
)
# TODO: win32: ARM_MINGW_SOURCES
AARCH64_SOURCES=(
  cpu_model.c \
  aarch64/fp_mode.c \
  aarch64/lse.S \
)
# TODO: mingw: AARCH64_MINGW_SOURCES=( aarch64/chkstk.S )

# # cmake: foreach(pat cas swp ldadd ldclr ldeor ldset) ...
# rm -f aarch64/outline_atomic_*.S
# cp aarch64/lse.S aarch64/outline_atomic.S.inc
# AARCH64_SOURCES+=( aarch64/outline_atomic.S.inc )
# for pat in cas swp ldadd ldclr ldeor ldset; do
#   for size in 1 2 4 8 16; do
#     for model in 1 2 3 4; do
#       if [ $pat = cas ] || [ $size != 16 ]; then
#         srcfile=aarch64/outline_atomic_${pat}${size}_${model}.S
#         echo "#define L_${pat}" >> $srcfile
#         echo "#define SIZE ${size}" >> $srcfile
#         echo "#define MODEL ${model}" >> $srcfile
#         echo '#include "outline_atomic.S.inc"' >> $srcfile
#         # OBJ_CFLAGS="-DL_${pat} -DSIZE=${size} -DMODEL=${model}"
#         # AARCH64_OBJECTS+=( "aarch64/lse.S.o:$OBJ_CFLAGS" )
#         AARCH64_SOURCES+=( $srcfile )
#       fi
#     done
#   done
# done

RISCV_SOURCES=(
  riscv/save.S \
  riscv/restore.S \
)
RISCV32_SOURCES=( riscv/mulsi3.S )
RISCV64_SOURCES=( riscv/muldi3.S )

# EXCLUDE_BUILTINS_VARS is a list of shell vars which in turn contains
# space-separated names of builtins to be excluded.
# The var names encodes the targets,
# e.g. "EXCLUDE_BUILTINS__macos" is for macos, any arch.
# e.g. "EXCLUDE_BUILTINS__macos__i386" is for macos, i386 only.
EXCLUDE_BUILTINS_VARS=()
#
# lib/builtins/Darwin-excludes/CMakeLists.txt
# cmake/Modules/CompilerRTDarwinUtils.cmake
#   macro(darwin_add_builtin_libraries)
#   function(darwin_find_excluded_builtins_list output_var)
# Note: The resulting names match source files without filename extension.
# For example, "addtf3" matches source file "addtf3.c".
EXCLUDE_APPLE_ARCHS_TO_CONSIDER=(i386 x86_64 arm64)
for d in Darwin-excludes; do
  for os in osx ios; do  # TODO: ios
    f=$d/$os.txt
    [ -f $f ] || continue
    sys=${os/osx/macos}
    var=EXCLUDE_BUILTINS__$sys
    declare $var=
    while read -r name; do
      declare $var="${!var} $name"
      declare EXCLUDE_FUN__any__${sys}__${name}=1
    done < $f
    [ -n "$var" ] && EXCLUDE_BUILTINS_VARS+=( $var )
    for arch in ${EXCLUDE_APPLE_ARCHS_TO_CONSIDER[@]}; do
      f=$(echo $d/${os}*-$arch.txt)
      [ -f $f ] || continue
      [[ "$f" != *"iossim"* ]] || continue
      var=EXCLUDE_BUILTINS__${sys}__${arch/arm64/aarch64}
      declare $var=
      while read -r name; do
        declare $var="${!var} $name"
        declare EXCLUDE_FUN__${arch}__${sys}__${name}=1
      done < $f
      [ -n "$var" ] && EXCLUDE_BUILTINS_VARS+=( $var )
    done
  done
done


rm -rf "$DESTDIR"
mkdir -p "$DESTDIR"

# aarch64: GENERIC_SOURCES + AARCH64_SOURCES
# arm:     GENERIC_SOURCES + ARM_SOURCES (+ ARM_EABI_SOURCES if not win32)
# i386:    GENERIC_SOURCES + X86_ARCH_SOURCES + I386_SOURCES
# x86_64:  GENERIC_SOURCES + X86_ARCH_SOURCES + X86_64_SOURCES
# riscv32: GENERIC_SOURCES + RISCV_SOURCES + RISCV32_SOURCES
# riscv64: GENERIC_SOURCES + RISCV_SOURCES + RISCV64_SOURCES
# wasm32:  GENERIC_SOURCES
# wasm64:  GENERIC_SOURCES

AARCH64_SOURCES_DIR=aarch64
ARM_SOURCES_DIR=arm
ARM_EABI_SOURCES_DIR=arm_eabi
X86_ARCH_SOURCES_DIR=x86
I386_SOURCES_DIR=i386
X86_64_SOURCES_DIR=x86_64
RISCV_SOURCES_DIR=riscv
RISCV32_SOURCES_DIR=riscv32
RISCV64_SOURCES_DIR=riscv64
DARWIN_SOURCES_DIR=any-macos

_copy ../../LICENSE.TXT                        "$DESTDIR"/LICENSE.TXT
_cpd "${GENERIC_SOURCES[@]}"  $(_deps .)       "$DESTDIR"
_cpd "${AARCH64_SOURCES[@]}"  $(_deps aarch64) "$DESTDIR"/$AARCH64_SOURCES_DIR
_cpd "${ARM_SOURCES[@]}"      $(_deps arm)     "$DESTDIR"/$ARM_SOURCES_DIR
_cpd "${ARM_EABI_SOURCES[@]}"                  "$DESTDIR"/$ARM_EABI_SOURCES_DIR
_cpd "${X86_ARCH_SOURCES[@]}"                  "$DESTDIR"/$X86_ARCH_SOURCES_DIR
_cpd "${I386_SOURCES[@]}"     $(_deps i386)    "$DESTDIR"/$I386_SOURCES_DIR
_cpd "${X86_64_SOURCES[@]}"   $(_deps x86_64)  "$DESTDIR"/$X86_64_SOURCES_DIR
_cpd "${RISCV_SOURCES[@]}"    $(_deps riscv)   "$DESTDIR"/$RISCV_SOURCES_DIR
_cpd "${RISCV32_SOURCES[@]}"                   "$DESTDIR"/$RISCV32_SOURCES_DIR
_cpd "${RISCV64_SOURCES[@]}"                   "$DESTDIR"/$RISCV64_SOURCES_DIR
_cpd "${DARWIN_SOURCES[@]}"                    "$DESTDIR"/$DARWIN_SOURCES_DIR

# update arrays to map to files in destdir
AARCH64_SOURCES=( "${AARCH64_SOURCES[@]/#*\//}" )
ARM_SOURCES=( "${ARM_SOURCES[@]/#*\//}" )
ARM_EABI_SOURCES=( "${ARM_EABI_SOURCES[@]/#*\//}" )
X86_ARCH_SOURCES=( "${X86_ARCH_SOURCES[@]/#*\//}" )
I386_SOURCES=( "${I386_SOURCES[@]/#*\//}" )
X86_64_SOURCES=( "${X86_64_SOURCES[@]/#*\//}" )
RISCV_SOURCES=( "${RISCV_SOURCES[@]/#*\//}" )
RISCV32_SOURCES=( "${RISCV32_SOURCES[@]/#*\//}" )
RISCV64_SOURCES=( "${RISCV64_SOURCES[@]/#*\//}" )
DARWIN_SOURCES=( "${DARWIN_SOURCES[@]/#*\//}" )

AARCH64_SOURCES=( "${AARCH64_SOURCES[@]/#/$AARCH64_SOURCES_DIR/}" )
ARM_SOURCES=( "${ARM_SOURCES[@]/#/$ARM_SOURCES_DIR/}" )
ARM_EABI_SOURCES=( "${ARM_EABI_SOURCES[@]/#/$ARM_EABI_SOURCES_DIR/}" )
X86_ARCH_SOURCES=( "${X86_ARCH_SOURCES[@]/#/$X86_ARCH_SOURCES_DIR/}" )
I386_SOURCES=( "${I386_SOURCES[@]/#/$I386_SOURCES_DIR/}" )
X86_64_SOURCES=( "${X86_64_SOURCES[@]/#/$X86_64_SOURCES_DIR/}" )
RISCV_SOURCES=( "${RISCV_SOURCES[@]/#/$RISCV_SOURCES_DIR/}" )
RISCV32_SOURCES=( "${RISCV32_SOURCES[@]/#/$RISCV32_SOURCES_DIR/}" )
RISCV64_SOURCES=( "${RISCV64_SOURCES[@]/#/$RISCV64_SOURCES_DIR/}" )
DARWIN_SOURCES=( "${DARWIN_SOURCES[@]/#/$DARWIN_SOURCES_DIR/}" )

for var in ${EXCLUDE_BUILTINS_VARS[@]}; do
  IFS=. read -r ign sys arch <<< "${var//__/.}"
  outfile="${arch:-any}-$sys.exclude"
  # echo "generate $(_relpath "$outfile")"
  echo ${!var} > "$outfile"
done

_popd

# ————————————————————————————————————————————————————————————————————————————————————
# generate header

_pushd "$DESTDIR"

echo "generating $(_relpath "$HEADER_FILE")"
cat << END > "$HEADER_FILE"
// librt sources
// Do not edit! Generated by ${SCRIPTNAME##$PROJECT/}
// SPDX-License-Identifier: Apache-2.0
END

# generate source list
echo "static const char* const librt_sources[] = {" >> "$HEADER_FILE"
n=0
for f in \
  "${GENERIC_SOURCES[@]}" \
  "${AARCH64_SOURCES[@]}" \
  "${ARM_SOURCES[@]}" \
  "${ARM_EABI_SOURCES[@]}" \
  "${X86_ARCH_SOURCES[@]}" \
  "${I386_SOURCES[@]}" \
  "${X86_64_SOURCES[@]}" \
  "${RISCV_SOURCES[@]}" \
  "${RISCV32_SOURCES[@]}" \
  "${RISCV64_SOURCES[@]}" \
  "${DARWIN_SOURCES[@]}" \
;do
  [ -f "$f" ] || _err "not found: $f (bug in $0)"  # sanity check
  echo "  \"$f\"," >> "$HEADER_FILE"

  # record source index for file
  var=${f//\//___}  # foo/bar/b-z.c => foo___bar___b-z.c
  var=${var//-/__}  # foo___bar___b-z.c => foo___bar___b__z.c
  var=${var//./_}  # foo___bar___b__z.c => foo___bar___b__z_c
  declare SRCIDX__${var}=$n
  # SOURCE_INDICES+=( "$f:$n" )
  (( n += 1 ))
done
BM_NBYTE=$(( ($n + 7) / 8 ))  # number of bytes needed for n bits
echo "};" >> "$HEADER_FILE"
echo "" >> "$HEADER_FILE"

_target_sources() {
  local indices=()
  local f srcidx ent var name excl_var_any excl_var_arch
  # local debug_files=()
  for f in *.c $arch/*.[csS] any-$sys/*.[csS] $arch-$sys/*.[csS]; do
    [ -f "$f" ] || continue
    name=${f:0:$(( ${#f} - 2 ))}
    name=${name##*/}
    excl_var_any=EXCLUDE_FUN__any__${sys}__${name}
    excl_var_arch=EXCLUDE_FUN__${arch}__${sys}__${name}
    if [ -n "${!excl_var_any:-}" -o -n "${!excl_var_arch:-}" ]; then
      # echo "skip $f"
      continue
    fi

    # lookup source index for file
    var=${f//\//___}
    var=${var//-/__}
    var=${var//./_}
    var=SRCIDX__${var}
    srcidx=${!var}

    indices+=( $srcidx )
    # debug_files+=( $f )
  done
  "$PROJECT"/etc/bitset -n "${indices[@]}"
  # echo " /* ${indices[@]} */"
  # echo " /* ${debug_files[@]} */"
  # echo ""
}

# compile helper program
HOST_ARCH=$(uname -m); HOST_ARCH=${HOST_ARCH/arm64/aarch64}
HOST_SYS=$(uname -s) # e.g. linux, macos
case "$HOST_SYS" in
  Darwin) HOST_SYS=macos ;;
  *)      HOST_SYS=$(awk '{print tolower($0)}' <<< "$HOST_SYS") ;;
esac
LLVMBOX="$PROJECT"/deps/llvmbox-$HOST_ARCH-$HOST_SYS
"$LLVMBOX"/bin/clang -std=c11 "$PROJECT"/etc/bitset.c -o "$PROJECT"/etc/bitset

# generate mapping for each target to corresponding sources
(
  echo "typedef struct {"
  echo "  targetdesc_t target;"
  echo "  u8 sources[$BM_NBYTE]; // bitmap index into librt_sources"
  echo "} librt_srclist_t;"
  echo "static const librt_srclist_t librt_srclist[] = {"
  for target_info in $(_co_targets); do  # ( "arch,sys,sysver,..." ... )
    IFS=, read -r arch sys sysver ign <<< "$target_info"
    if [ "$sys" = none ] && [ "$arch" != wasm32 -a "$arch" != wasm64 ]; then
      # no librt support for "none" with the exception of wasm
      continue
    fi
    printf "  {{ARCH_%s, SYS_%s, \"%s\"}, {" $arch $sys $sysver
    _target_sources
    echo "}},"
  done
  echo "};"
) >> "$HEADER_FILE"
