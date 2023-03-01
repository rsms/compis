PWD0=${PWD0:-$PWD}
SCRIPT_FILE=${BASH_SOURCE[0]}
PROJECT=$(dirname $(dirname $(realpath "$SCRIPT_FILE")))
DEPS_DIR="$PROJECT/deps"
OUT_DIR="$PROJECT/out"
DOWNLOAD_DIR="$DEPS_DIR/download"
LLVMBOX="$DEPS_DIR/llvmbox"

_err() { echo "$0:" "$@" >&2; exit 1; }

_relpath() { # <path>
  case "$1" in
    "$PWD0/"*) echo "${1##$PWD0/}" ;;
    "$PWD0")   echo "." ;;
    # "$HOME/"*) echo "~${1:${#HOME}}" ;;
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

_copy() {
  printf "copy"
  local past=
  for arg in "$@"; do case "$arg" in
    -*) [ -z "$past" ] || printf " %s" "$(_relpath "$arg")";;
    --) past=1 ;;
    *)  printf " %s" "$(_relpath "$arg")" ;;
  esac; done
  printf "\n"
  cp -R "$@"
}

_cpd() {
  echo "copy $(($#-1)) files to $(_relpath "${@: -1}")/"
  mkdir -p "${@: -1}"
  cp -r "$@"
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

_co_targets() {
  local TARGETS=()  # ( "arch,sys,sysver" , ... )
  while IFS=, read -r arch sys sysver rest; do
    TARGETS+=( "${arch:7},$sys,$sysver" )
  done <<< "$(awk '$1 ~ /^TARGET\(/' "$PROJECT/src/targets.h" | sed -E 's/[ "]//g')"
  echo "${TARGETS[@]}"
}
