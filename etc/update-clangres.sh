#!/usr/bin/env bash
set -euo pipefail
source "$(dirname "$0")/lib.sh"
cd "$PROJECT"

[ -d "$LLVMBOX" ] || "$BASH" build.sh -config

rm -rf lib/clangres
mkdir -p lib/clangres
_copy "$LLVMBOX"/lib/clang/*/include lib/clangres
