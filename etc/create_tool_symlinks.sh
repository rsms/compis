#!/usr/bin/env bash
set -euo pipefail
source "$(dirname "$0")/lib.sh"

ARGS=()

while [[ $# -gt 0 ]]; do case "$1" in
  -h|-help|--help) cat << _END
Create symlinks for common tools like cc and ar
Usage: $0 <coexe> [<dir>]
<coexe> Path to an existing compis executable to point links to
<dir>   Where to create links. Defaults to dirname(<coexe>)
_END
    exit ;;
  --) shift; ARGS+=( "$@" ); break ;;
  -*) _err "Unknown option: $1" ;;
  *)  ARGS+=( "$1" ); shift ;;
esac; done

[ ${#ARGS[@]} -gt 0 ] || _err "missing <coexe>"
[ ${#ARGS[@]} -lt 3 ] || _err "unexpected extra argument: ${ARGS[2]}"

_create_tool_symlinks "${ARGS[@]}"
