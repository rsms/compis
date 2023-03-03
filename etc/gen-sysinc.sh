#!/usr/bin/env bash
set -euo pipefail
source "$(dirname "$0")/lib.sh"

_pushd "$PROJECT"

rm -rf lib/sysinc
cp -R lib/sysinc-src lib/sysinc

"$LLVMBOX"/bin/llvmbox-dedup-target-files lib/sysinc

echo "Removing any dotfiles and empty directories"
find lib/sysinc \( -type f -name '.*' -o -type d -empty \) -delete -print
