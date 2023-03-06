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

_copy() { # <arg to cp> ...
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

_symlink() { # <linkfile-to-create> <target>
  echo "symlink $(_relpath "$1") -> $(_relpath "$2")"
  [ ! -e "$1" ] || [ -L "$1" ] || _err "$(_relpath "$1") exists (not a link)"
  rm -f "$1"
  ln -fs "$2" "$1"
}

_hascmd() { # <cmd>
  command -v "$1" >/dev/null || return 1
}

_needcmd() {
  while [ $# -gt 0 ]; do
    _hascmd "$1" || _err "$1 not found in PATH"
    shift
  done
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
  command -v curl >/dev/null &&
    curl -L '-#' -o "$outfile" "$url" ||
    wget -O "$outfile" "$url"
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

_create_tar_xz_from_dir() { # <srcdir> <dstfile>
  command -v tar >/dev/null || _err "can't find \"tar\" in PATH"
  local archive=$2
  [[ "$archive" == *".tar.xz" ]] || _err "$archive doesn't end with .tar.xz"
  case "$archive" in
    /*) ;;
    *)  archive="$PWD/$archive" ;;
  esac
  mkdir -p "$(dirname "$archive")"
  local srcdir="$(realpath "$1")"
  if command -v xz >/dev/null; then
    tar -C "$(dirname "$srcdir")" -c "$(basename "$srcdir")" \
    | xz -9 -f -T0 -v > "$archive"
  else
    XZ_OPT='-9 -T0' \
    tar -C "$(dirname "$srcdir")" -cJpf "$archive" "$(basename "$srcdir")"
  fi
}

_co_targets() { # -> "arch,sys,sysver,intsize,ptrsize,llvm_triple" ...
  awk '$1 ~ /^TARGET\(/' "$PROJECT/src/targets.h" |
  sed -E 's/[ "]//g' |
  sed -E 's/^TARGET\((.+)\)$/\1/'
}

_version_gte() { # <v> <minv>
  local v1 v2 v3 min_v1 min_v2 min_v3
  IFS=. read -r v1 v2 v3 <<< "$1"
  IFS=. read -r min_v1 min_v2 min_v3 <<< "$2"
  [ -n "$min_v2" ] || min_v2=0
  [ -n "$min_v3" ] || min_v3=0
  [ -n "$v2" ] || v2=$min_v2
  [ -n "$v3" ] || v3=$min_v3
  # echo "v   $v1 $v2 $v3"
  # echo "min $min_v1 $min_v2 $min_v3"
  if [ "$v1" -lt "$min_v1" ]; then return 1; fi
  if [ "$v1" -gt "$min_v1" ]; then return 0; fi
  if [ "$v2" -lt "$min_v2" ]; then return 1; fi
  if [ "$v2" -gt "$min_v2" ]; then return 0; fi
  if [ "$v3" -lt "$min_v3" ]; then return 1; fi
  if [ "$v3" -gt "$min_v3" ]; then return 0; fi
}
