PWD0=${PWD0:-$PWD}
LIBSH_FILE=${BASH_SOURCE[0]}
SCRIPT_FILE=${BASH_SOURCE[$(( ${#BASH_SOURCE[@]} - 1 ))]}
PROJECT=$(dirname $(dirname $(realpath "$LIBSH_FILE")))
DEPS_DIR="$PROJECT/deps"
OUT_DIR="$PROJECT/out"
DOWNLOAD_DIR="$DEPS_DIR/download"
LLVMBOX="$DEPS_DIR/llvmbox"

_err()  { echo "$0:" "$@" >&2; exit 1; }
_warn() { echo "$0: warning:" "$@" >&2; }

_relpath() { # <path>
  case "$1" in
    "$PWD0/"*) echo "${1##$PWD0/}" ;;
    "$PWD0")   echo "." ;;
    # "$HOME/"*) echo "~${1:${#HOME}}" ;;
    *)         echo "$1" ;;
  esac
}

_pushd() {
  local wd="$PWD"
  pushd "$1" >/dev/null
  [ "$PWD" = "$wd" -o "$PWD" = "$PWD0" ] || echo "cd $(_relpath "$PWD")"
}

_popd() {
  local wd="$PWD"
  popd >/dev/null
  [ "$PWD" = "$wd" -o "$PWD" = "$PWD0" ] || echo "cd $(_relpath "$PWD")"
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
  if [ -L "$1" -a "$(readlink "$1")" = "$2" ]; then
    return 0
  fi
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

_sha256() { # [<file>]
  sha256sum "$@" | cut -d' ' -f1
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

_download_and_extract_tar() { # <url> <outdir> [<sha256> | <sha512> [<tarfile>]]
  local url=$1
  local outdir=$2
  local checksum=${3:-}
  local tarfile="${4:-$DOWNLOAD_DIR/$(basename "$url")}"
  _download "$url" "$tarfile" "$checksum"
  _extract_tar "$tarfile" "$outdir"
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

_sysinc_src_githash() {
  if [ -d "$PROJECT/.git" ]; then
    git log -n1 --format=%H -- "$PROJECT/lib/sysinc-src"
  fi
}

_need_regenerate_sysinc_dir() {
  if [ ! -d "$PROJECT/lib/sysinc" ]; then
    return 0
  fi
  # note: if the filename changes, also update gen-sysinc.sh
  if [ "$(cat "$PROJECT/lib/sysinc/.srcver")" != "$(_sysinc_src_githash)" ]; then
    return 0
  fi
  return 1
}

_regenerate_sysinc_dir() {
  printf "Generating lib/sysinc from lib/sysinc-src ..."
  local LOGFILE="$OUT_DIR/gen-sysinc.log"
  if ! $BASH "$PROJECT/etc/gen-sysinc.sh" > "$LOGFILE" 2>&1; then
    echo " failed. See $(_relpath "$LOGFILE")" >&2
    exit 1
  fi
  echo
}

_abspath() { # <path>
  local path="${1%/}" # "%/" = shave off any trailing "/"
  case "$path" in
    "")  echo "/" ;;
    /*)  echo "$path" ;;
    ./*) echo "$PWD/${path:2}" ;;
    *)   echo "$PWD/$path" ;;
  esac
}

_relative_path() { # <basedir> <subject>
  local basedir="$(_abspath "$1")"
  local subject="$(_abspath "$2")"
  case "$subject" in
    "$basedir")   echo "."; return 0 ;;
    "$basedir/"*) echo "${subject:$(( ${#basedir} + 1 ))}"; return 0 ;;
  esac
  # TODO: calculate "../" path
  echo "$subject"
}

_create_tool_symlinks() { # <coexe> [<dir>]
  local coexe="$(realpath "$1" 2>/dev/null || _abspath "$1")"
  local dir="$(realpath "${2:-"$(dirname "$coexe")"}")"
  local target="$(_relative_path "$dir" "$coexe")"
  for name in \
    cc \
    c++ \
    ar \
    as \
    ranlib \
    ld.lld \
    ld64.lld \
    wasm-ld \
    lld-link \
  ;do
    _symlink "$dir/$name" "$target"
  done
}
