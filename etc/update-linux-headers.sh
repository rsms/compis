#!/bin/bash
set -euo pipefail
source "$(dirname "$0")/lib.sh"

LINUX_VERSION=6.6.6
LINUX_VERSION_MAJOR=${LINUX_VERSION%%.*}
LINUX_SHA256=ebf70a917934b13169e1be5b95c3b6c2fea5bc14e6dc144f1efb8a0016b224c8
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

if [ ! -f "$LINUX_SRCDIR/Makefile" ]; then
  _download \
    https://mirrors.kernel.org/pub/linux/kernel/v${LINUX_VERSION_MAJOR}.x/linux-$LINUX_VERSION.tar.xz \
    "$LINUX_SHA256" \
    "$DOWNLOAD_DIR/linux-$LINUX_VERSION.tar.xz"
  _extract_tar \
    "$DOWNLOAD_DIR/linux-$LINUX_VERSION.tar.xz" \
    "$LINUX_SRCDIR"
fi

_pushd "$LINUX_SRCDIR"

for arch in arm arm64 riscv x86 i386; do
  co_arch=${arch/arm64/aarch64}
  co_arch=${co_arch/x86/x86_64}
  co_arch=${co_arch/riscv/riscv64}
  DESTDIR="$SYSINC_SRCDIR/"${co_arch}-linux
  rm -rf "$DESTDIR.tmp"
  # note: do not use -j here!
  echo "make ARCH=$arch headers_install > $(_relpath "$DESTDIR.log")"
  (
    make ARCH=$arch INSTALL_HDR_PATH="$DESTDIR.tmp" headers_install
    rm -rf "$DESTDIR"
    mv "$DESTDIR.tmp/include" "$DESTDIR"
    rm -rf "$DESTDIR.tmp"
  ) > "$DESTDIR.log" 2>&1 &
done
wait
rm -f "$SYSINC_SRCDIR/"*-linux.log

_popd
# rm -rf "$LINUX_SRCDIR"

# regenerate lib/sysinc to avoid weird bugs from building with old versions
_regenerate_sysinc_dir
