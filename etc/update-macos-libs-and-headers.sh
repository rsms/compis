#!/usr/bin/env bash
set -euo pipefail
source "$(dirname "$0")/lib.sh"
cd "$PROJECT"
#
# You need to run this script on both an x86_64 mac an an arm one
# since headers are resolved by clang.
#
# Manual first steps:
# 1. Download all SDKs you'd like to import from
#    https://developer.apple.com/download/all/?q=command%20line
#    Note that you can search for specific OS versions, e.g.
#    https://developer.apple.com/download/all/?q=command%20line%2010.13
# 2. Mount all downloaded disk images,
#    so that you have e.g. "/Volumes/Command Line Developer Tools"
# 3. Run this script
#
# Run with -dry-run to just find headers, without updating sysinc.
#
[ "$(uname -s)" = Darwin ] || _err "must run this script on macOS"
case "${1:-}" in
  -h|--help) echo "usage: $0 [-dry-run]"; exit 0;;
esac

PBZX=$OUT_DIR/pbzx
CLT_TMP_DIR="$OUT_DIR/apple-clt-tmp"
LIBLZMA=
SDKS=()
SDK_VERSIONS=()
HOST_ARCH=$(uname -m); HOST_ARCH=${HOST_ARCH/arm64/aarch64}
HEADERS_OUTFILE="$OUT_DIR/macos-headers"

case "$HOST_ARCH" in
  x86_64)  TARGET_SYS_MINVERSION=10.15 ;;
  aarch64) TARGET_SYS_MINVERSION=11.0 ;;
  *) _err "unsupported arch: $(uname -m)" ;;
esac
IFS=. read -r MIN_VER_MAJ MIN_VER_MIN <<< "$TARGET_SYS_MINVERSION"

COMPIS="$OUT_DIR/opt-$HOST_ARCH-$HOST_SYS/co"

_require_compis() {
  if [ ! -f "$COMPIS" ]; then
    echo "building compis"
    $BASH "$PROJECT/build.sh"
  fi
}

_require_pbzx() {
  if [ ! -x "$PBZX" ]; then
    printf "building $(_relpath "$PBZX") ..."
    local LOGFILE="$OUT_DIR/build-pbzx.log"
    if ! $BASH "$PROJECT/etc/build-pbzx.sh" > "$LOGFILE" 2>&1; then
      echo " failed. See $(_relpath "$LOGFILE")" >&2
      exit 1
    fi
    echo "done."
  fi
}

_strset_add() { # <setvar> <value>  => 0 if added, 1 if duplicate
  local setvar=$1
  local valkey=$2
  re='(.*)[\.\-](.*)'  # e.g. "macos.10.15" => "macos_10_15"
  while [[ $valkey =~ $re ]]; do
    valkey=${BASH_REMATCH[1]}_${BASH_REMATCH[2]}
  done
  local key="keyset_${setvar}_${valkey}"
  [ -z "${!key:-}" ] || return 1
  eval "$key=1" # can't use 'declare -rg "$key=1"' in bash<4
  eval "$setvar+=( $2 )"
}

_sdk_version() { # <sdkpath>
  local re='Mac[a-zA-Z]+([0-9]+\.[0-9]+)'
  while [[ $1 =~ $re ]]; do
    echo ${BASH_REMATCH[1]}
    return 0
  done
}

_is_sdk_version_gte_minver() { # <version>
  local ver_maj ver_min
  IFS=. read -r ver_maj ver_min <<< "$1"
  if [ $ver_maj -lt $MIN_VER_MAJ ] ||
     [ $ver_maj -eq $MIN_VER_MAJ -a $ver_min -lt $MIN_VER_MIN ]; then
    return 1
  fi
  return 0
}

_add_sdks() { # <path> ...
  local d
  local found
  local ver
  local path
  for d in "$@"; do
    # Resolve symlinks since some versioned SDK dirs point to unversioned dirs, e.g.
    #   MacOSX10.13.sdk -> MacOSX.sdk
    # For newer versions it's the other way around and this has no effect.
    [ -d "$d" ] || continue
    path="$(realpath "$d")"
    ver=$(_sdk_version "$d" || true)
    if [ -z "$ver" ]; then
      echo "ignoring version-less '$d'"
      continue
    fi
    if ! _is_sdk_version_gte_minver "$ver"; then
      echo "ignoring SDK $ver; version older than TARGET_SYS_MINVERSION ($TARGET_SYS_MINVERSION)"
      continue
    fi
    found=
    for v in "${SDK_VERSIONS[@]:-}"; do
      if [ "$v" = "$ver" ]; then
        found=1
        break
      fi
    done
    if [ -z "$found" ]; then
      SDKS+=( "$ver:$path" )
      SDK_VERSIONS+=( "$ver" )
    fi
  done
}

_import_headers() { # <sdk-dir> <sysversion> <sdkver>
  local sdkdir=$1
  local sysver=$2 # M
  local sdkver=$3 # M.m
  local sysroot_name="$HOST_ARCH-macos.$sysver"
  local dst_incdir="$PROJECT/lib/sysinc-src/$sysroot_name"
  local name framework dstpath
  rm -rf "$dst_incdir"
  mkdir -p "$dst_incdir"

  _require_compis

  echo "  finding headers"
  "$COMPIS" cc \
    -nostdinc \
    --sysroot="$sdkdir/" \
    --target=x86_64-macos.$sysver \
    "-isystem$sdkdir/usr/include" \
    -o "$HEADERS_OUTFILE" -c "$PROJECT/etc/macos-headers.c" \
    -MD -MV -MF "$HEADERS_OUTFILE.d"

  echo "  copying headers -> $(_relpath "$dst_incdir")/"
  while read -r line; do
    [[ "$line" != *":"* ]] || continue # ignore first line
    [[ "$line" != *"/etc/macos-headers.c"* ]] || continue
    [[ "$line" != *"/clang/"* ]] || continue
    [[ "$line" != *"/clangres/"* ]] || continue
    path=${line/ \\/} # "foo \" => "foo"

    if [[ "$path" == *"/usr/include/"* ]]; then
      name="${path/*\/usr\/include\//}" # /a/b/usr/include/foo/bar.h => foo/bar.h
    elif [[ "$path" == *".framework/Headers/"* ]]; then
      name="${path/*.framework\/Headers\//}"
      framework=$(echo "$path" | sed -E 's/\/.+\/([^\/]+)\.framework\/Headers.+$/\1/')
      name="$framework/$name" # e.g. CoreFoundation/CFBase.h
    else
      _err "unexpected path: $path"
    fi
    [[ "$name" != "/"* ]] || _err "unexpected path: $line"

    dstpath="$dst_incdir/$name"
    [ ! -f "$dstpath" ] || _err "duplicate file: $dstpath (2nd src: $path)"
    $BASH -c "mkdir -p '$(dirname "$dstpath")' &&
      install -m 0644 '$path' '$dstpath'" &
  done < "$HEADERS_OUTFILE.d"
  wait
}

_import_libs() { # <sdk-dir> <sysversion>
  local sdkdir=$1
  local sysver=$2
  local sysroot_name dst_libdir name ent alias src srcdir dst
  local libs_anyarch=( libSystem.tbd )
  local cs1 cs2
  local do_copy

  # import libs for any arch
  sysroot_name="any-macos.$sysver"
  dst_libdir="$PROJECT/lib/darwin/$sysroot_name"
  for name in "${libs_anyarch[@]}"; do
    srcdir="$sdkdir/usr/lib"
    src="$srcdir/$name"
    [ -e "$src" ] || continue
    src="$(realpath "$src")"
    dst="$dst_libdir/$name"
    do_copy=1

    if [ -e "$dst" ]; then
      cs1=$(sha256sum "$src" | cut -d' ' -f1)
      cs2=$(sha256sum "$dst_libdir/$name" | cut -d' ' -f1)
      if [ "$cs1" != "$cs2" ]; then
        cat <<- END >&2

  ——————————————————————————————————— note ————————————————————————————————————
  Overwriting $(_relpath "$dst") which is different from
  $(_relpath "$src")
  —————————————————————————————————————————————————————————————————————————————

END
      else
        do_copy=0
      fi
    fi
    if [ $do_copy -eq 1 ]; then
      echo "  copying lib $name -> $(_relpath "$dst")"
      mkdir -p "$dst_libdir"
      install -m 0644 "$src" "$dst"
    fi

    # match symlinks
    for ent in "$srcdir/"*; do
      [[ "$ent" != *".1.tbd" ]] || continue
      [[ "$ent" != *".A.tbd" ]] || continue
      [[ "$ent" != *".B.tbd" ]] || continue
      [[ "$ent" != *".C.tbd" ]] || continue
      [ -L "$ent" ] || continue
      dst="$(readlink "$ent")"
      [ "$dst" = "$name" ] || continue
      alias="$(basename "$ent")"
      echo "  symlink $alias -> $name"
      ln -sf "$name" "$dst_libdir/$alias"
    done
  done
}

_import_sdk() { # <path> <version>
  local sdkdir=$1
  local sdkver=$2 # M.m
  local sysver # M
  IFS=. read -r sysver ign <<< "$sdkver"
  local sysroot_name="$HOST_ARCH-macos.$sysver"
  echo "importing $sysroot_name"

  [ -d "$sdkdir" ] || _err "'$sdkdir' is not a directory"
  [[ "$(basename "$sdkdir" .sdk)" == "MacOSX"* ]] ||
    _err "SDK doesn't start with 'MacOSX'; bailing out ($sdkdir)"

  _import_headers "$sdkdir" "$sysver" "$sdkver"
  _import_libs    "$sdkdir" "$sysver"
}

# ———————————————————————————————————————————————————————————————————————————————————

mkdir -p "$CLT_TMP_DIR"

# extract mounted "Command Line Developer Tools" installers
for d in "/Volumes/Command Line Developer Tools"*; do
  [ -d "$d" ] || continue
  ID=
  DISK_IMAGE="$(hdiutil info | grep -B20 "$d" | grep image-path |
    awk '{print $3 $4 $5 $6 $7}' || true)"
  if [ -f "$DISK_IMAGE" ]; then
    ID=$(sha1sum "$DISK_IMAGE" | cut -d' ' -f1)
  else
    ID=$(sha1sum <<< "$d" | cut -d' ' -f1)
  fi
  SUBDIR="$CLT_TMP_DIR/$ID"
  if [ -f "$SUBDIR/processed.mark" ]; then
    echo "skipping already-processed $(_relpath "$SUBDIR")"
    continue
  fi
  echo "extracting $(_relpath "$d")"
  mkdir -p "$SUBDIR"
  _pushd "$SUBDIR"
  pkg="$(echo "$d/Command Line Tools"*.pkg)"
  if [ -f "$pkg" ]; then
    # extra .pkg wrapper
    echo "  xar -xf $pkg"
    xar -xf "$pkg"
  else
    # pre 10.14 SDKs didn't have that wrapper
    echo "  cp" "$d"/*_SDK_macOS*.pkg "."
    cp -a "$d"/*_SDK_macOS*.pkg .
  fi
  _require_pbzx
  for f in *_mac*_SDK.pkg *_SDK_macOS*.pkg; do
    [ -e "$f" ] || continue
    payload_file="$f/Payload"
    if [ -f "$f" ]; then
      echo "  xar -xf $f"
      xar -xf "$f"
      payload_file="Payload"
    fi
    echo "  $PBZX -n $payload_file | cpio -i"
    "$PBZX" -n "$payload_file" | cpio -i 2>/dev/null &
  done
  printf "  ..." ; wait ; echo
  rm -rf Payload Bom PackageInfo Distribution Resources *.pkg
  _popd
  touch "$SUBDIR/processed.mark"
done

# add SDKs from extracted "Command Line Developer Tools" installers
_add_sdks $CLT_TMP_DIR/*/Library/Developer/CommandLineTools/SDKs/MacOSX*.*.sdk

# add system-installed SDKs from well-known paths
_add_sdks /Library/Developer/CommandLineTools/SDKs/MacOSX*.*.sdk
_add_sdks /Applications/Xcode.app/Contents/Developer/Platforms/MacOSX.platform/Developer/SDKs/MacOSX*.*.sdk

# add system-installed SDKs from xcrun
if command -v xcrun >/dev/null; then
  _add_sdks "$(xcrun --show-sdk-path)"
  _add_sdks "$(xcrun -sdk macosx --show-sdk-path)"
fi

# did we find any SDKs?
[ -n "$SDKS" ] || _err "no SDKs found"

# sort SDKs by version so that a later minor version is processed after an earlier one
SDKS_TMP=()
for ver_path in "${SDKS[@]}"; do
  IFS=: read -r ver path <<< "$ver_path"
  IFS=. read -r v1 v2 <<< "$ver"
  SDKS_TMP+=( $(printf "%02d%02d:%s" $v1 $v2 "$ver_path") )
done
IFS=$'\n' SDKS_SORTED=($(sort -r <<< "${SDKS_TMP[*]}")); unset IFS

# Filter SDKs: select only the most recent SDK per major version.
SDKS=()
SDK_MAJOR_VERSIONS=()
for key_ver_path in "${SDKS_SORTED[@]}"; do
  IFS=: read -r key ver path <<< "$key_ver_path" # "10.15:/foo/bar"
  IFS=. read -r ver_key ver_min <<< "$ver"
  if _strset_add SDK_MAJOR_VERSIONS "$ver_key"; then
    SDKS+=( "$ver:$path" )
  fi
done

# print what will be imported
echo "Importing ${#SDKS[@]} macOS SDKs:"
for d in "${SDKS[@]}"; do
  IFS=: read -r ver path <<< "$d"
  if [[ "$path" == "$CLT_TMP_DIR/"* ]]; then
    SUBDIR="${path:$(( ${#CLT_TMP_DIR} + 1 ))}"
    SUBDIR="$CLT_TMP_DIR/${SUBDIR%%/*}"
  else
    SUBDIR="$path"
  fi
  printf -- "- %- 5s  %-15s  %s\n" \
    "$ver" "$(basename "$path")" "$(_relpath "$SUBDIR")"
done

if [ "${1:-}" = "-dry-run" ]; then
  exit 0
fi

# actually import
for d in "${SDKS[@]}"; do
  IFS=: read -r ver path <<< "$d"
  _import_sdk "$path" "$ver"
done

_regenerate_sysinc_dir
