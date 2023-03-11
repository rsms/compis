#!/bin/bash
set -euo pipefail
source "$(dirname "$0")/lib.sh"

LINUX_VERSION=6.1.7
LINUX_VERSION_MAJOR=${LINUX_VERSION%%.*}
LINUX_SHA256=4ab048bad2e7380d3b827f1fad5ad2d2fc4f2e80e1c604d85d1f8781debe600f
LINUX_SRCDIR="$DEPS_DIR/linux-$LINUX_VERSION"
SYSINC_SRCDIR="$PROJECT/lib/sysinc-src"

# headers_install runs some sed scripts which are not compatible with BSD sed.
# Even more importantly, the "ARCH=x86 headers_install" target compiles a program
# arch/x86/tools/relocs_32.o which needs elf.h from the host system.
[ "$(uname -s)" = Linux ] || _err "must run this script on Linux"

if [ "$(uname -s)" != Linux ]; then
  # headers_install runs some sed scripts which are not compatible with BSD sed.
  # On macOS Homebrew you can install GNU sed ("gsed") with "brew install gnu-sed".
  if [[ "$(sed --version 2>/dev/null)" != *"GNU sed"* ]]; then
    mkdir -p "$DEPS_DIR/linux-utils"
    export PATH="$DEPS_DIR/linux-utils:$PATH"
    command -v gsed >/dev/null || _err "gsed not found in PATH"
    ln -fs "$(command -v gsed)" "$DEPS_DIR/linux-utils/sed"
  fi
fi

[ ! -f "$LINUX_SRCDIR/Makefile" ] &&
  _download \
    https://mirrors.kernel.org/pub/linux/kernel/v${LINUX_VERSION_MAJOR}.x/linux-$LINUX_VERSION.tar.xz \
    "$DOWNLOAD_DIR/linux-$LINUX_VERSION.tar.xz" \
    "$LINUX_SHA256" &&
  _extract_tar \
    "$DOWNLOAD_DIR/linux-$LINUX_VERSION.tar.xz" \
    "$LINUX_SRCDIR"

_pushd "$LINUX_SRCDIR"

for arch in arm arm64 riscv x86; do
  co_arch=${arch/arm64/aarch64}
  co_arch=${co_arch/x86/x86_64}
  co_arch=${co_arch/riscv/riscv64}
  DESTDIR="$SYSINC_SRCDIR/"${co_arch}-linux
  rm -rf "$DESTDIR.tmp"
  # note: do not use -j here!
  echo "make ARCH=$arch headers_install > $(_relpath "$DESTDIR.log")"
  # (
    make ARCH=$arch INSTALL_HDR_PATH="$DESTDIR.tmp" headers_install
    rm -rf "$DESTDIR"
    mv "$DESTDIR.tmp/include" "$DESTDIR"
    rm -rf "$DESTDIR.tmp"
  # ) > "$DESTDIR.log" 2>&1 &
done
wait
rm -f "$SYSINC_SRCDIR/"*-linux.log

_popd
# rm -rf "$LINUX_SRCDIR"

# regenerate lib/sysinc to avoid weird bugs from building with old versions
_regenerate_sysinc_dir
