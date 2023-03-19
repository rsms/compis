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

# note: if the filename changes, also update _need_regenerate_sysinc_dir in lib.sh.
# note: we assume dist.sh excludes ".*" files from the package.
_sysinc_src_githash > lib/sysinc/.srcver

# # deduplicate files using hardlinks
# fdupes="$DEPS_DIR/fdupes/fdupes"
# if [ ! -x "$fdupes" ]; then
#   git clone https://github.com/tobiasschulz/fdupes.git "$DEPS_DIR/fdupes"
#   CO="$PROJECT/out/opt/co"
#   [ ! -x "$CO" ] && "$PROJECT/build.sh" -no-lto
#   CC="$CO cc" \
#   make -C "$DEPS_DIR/fdupes" -j$(nproc)
# fi

# # create tar files
# _pushd lib/sysinc
# for f in *; do
#   [ -d "$f" ] || continue
#   echo "creating $(basename $f).tar"
#   "$fdupes" --recurse --linkhard "$f"
#   tar -f "$(basename $f).tar" -c -b 8 -C "$f" . &
# done
# wait
