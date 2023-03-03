#!/usr/bin/env bash
set -euo pipefail
source "$(dirname "$0")/lib.sh"

_pushd "$PROJECT"

rm -rf lib/sysinc
_copy lib/sysinc-src lib/sysinc

"$LLVMBOX"/bin/llvmbox-dedup-target-files lib/sysinc

# when/if we support RISC-V 32: (note that musl does not yet support riscv32)
# _symlink lib/sysinc/riscv32-linux riscv64-linux

echo "Removing any dotfiles and empty directories"
find lib/sysinc \( -type f -name '.*' -o -type d -empty \) -delete -print
