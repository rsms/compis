#!/usr/bin/env bash
#
# pbzx is a development tool used to update macos headers & libs
#
set -euo pipefail
source "$(dirname "$0")/lib.sh"
cd "$PROJECT"

if [ ! -f "$OUT_DIR/opt/co" ]; then
  echo "building compis"
  $BASH "$PROJECT/build.sh"
fi

PBZX="$OUT_DIR/pbzx"
PBZX_SRC="$PROJECT/etc/pbzx.c"

XZ_VERSION=5.2.5
XZ_SHA256=3e1e518ffc912f86608a8cb35e4bd41ad1aec210df2a47aaa1f95e7f5576ef56
XZ_SRC="$DEPS_DIR/xz-$XZ_VERSION"
XZ="$XZ_SRC/build"

OPENSSL_VERSION=1.1.1s
OPENSSL_SHA256=c5ac01e760ee6ff0dab61d6b2bbd30146724d063eb322180c6f18a6f74e4b6aa
OPENSSL_SRC="$DEPS_DIR/openssl-$OPENSSL_VERSION"
OPENSSL="$OPENSSL_SRC/build"

LIBXAR_SRC="$PROJECT/etc/libxar"
LIBXAR="$DEPS_DIR/libxar"

MACOS_SDK=$(xcrun -sdk macosx --show-sdk-path)
[ -d "$MACOS_SDK" ] || _err "xcrun -sdk macosx --show-sdk-path failed"

# ————————————————————————————————————————————————————————————————————————————————————
# xz (liblzma)

if [ ! -f "$XZ/lib/liblzma.a" ]; then
  _download_and_extract_tar \
    https://tukaani.org/xz/xz-$XZ_VERSION.tar.xz \
    "$XZ_SRC" \
    "$XZ_SHA256"

  _pushd "$XZ_SRC"
  echo "building xz (liblzma)"

  CC="$OUT_DIR/opt/co cc" \
  AR="$OUT_DIR/opt/co ar" \
  RANLIB="$OUT_DIR/opt/co ranlib" \
  ./configure \
    --prefix= \
    --enable-static \
    --disable-shared \
    --disable-rpath \
    --disable-werror \
    --disable-doc \
    --disable-nls \
    --disable-dependency-tracking \
    --disable-xz \
    --disable-xzdec \
    --disable-lzmadec \
    --disable-lzmainfo \
    --disable-lzma-links \
    --disable-scripts \
    --disable-doc

  make -j$(nproc)

  rm -rf "$XZ"
  mkdir -p "$XZ"/{lib,include}
  make DESTDIR="$XZ" -j$(nproc) install

  # remove libtool file which is just going to confuse libxml2
  rm -fv "$XZ"/lib/*.la

  _popd
fi

# ————————————————————————————————————————————————————————————————————————————————————
# openssl (libcrypto)

if [ ! -f "$OPENSSL/lib/libcrypto.a" ]; then
  _download_and_extract_tar \
    https://www.openssl.org/source/openssl-$OPENSSL_VERSION.tar.gz \
    "$OPENSSL_SRC" \
    "$OPENSSL_SHA256"

  _pushd "$OPENSSL_SRC"
  echo "building openssl (libcrypto)"

  CC="$OUT_DIR/opt/co cc" \
  AR="$OUT_DIR/opt/co ar" \
  RANLIB="$OUT_DIR/opt/co ranlib" \
  CFLAGS="-I$LLVMBOX/include -isystem$MACOS_SDK/usr/include -Wno-unused-command-line-argument" \
  LDFLAGS="-L$LLVMBOX/lib -L$MACOS_SDK/usr/lib" \
  ./config \
    --prefix=/ \
    --libdir=lib \
    --openssldir=/etc/ssl \
    no-zlib \
    no-shared \
    no-async \
    no-comp \
    no-idea \
    no-mdc2 \
    no-rc5 \
    no-ec2m \
    no-sm2 \
    no-sm4 \
    no-ssl2 \
    no-ssl3 \
    no-seed \
    no-weak-ssl-ciphers \
    -Wa,--noexecstack

  make -j$(nproc)

  rm -rf "$OPENSSL"
  mkdir -p "$OPENSSL"
  make DESTDIR="$OPENSSL" -j$(nproc) install_sw
  rm -rf "$OPENSSL"/bin "$OPENSSL"/lib/pkgconfig

  _popd
fi

# ————————————————————————————————————————————————————————————————————————————————————
# libxar

if [ ! -f "$LIBXAR/lib/libxar.a" ]; then
  _pushd "$LIBXAR_SRC"
  echo "building libxar"

  XAR_CFLAGS=(
    -Wno-deprecated-declarations \
    "-I$XZ/include" \
    "-I$OPENSSL/include" \
    "-I$LLVMBOX/include" \
  )
  XAR_LDFLAGS=(
    "-L$XZ/lib" \
    "-L$OPENSSL/lib" \
    "-L$LLVMBOX/lib" \
  )

  CC="$OUT_DIR/opt/co cc" \
  AR="$OUT_DIR/opt/co ar" \
  RANLIB="$OUT_DIR/opt/co ranlib" \
  CFLAGS="${XAR_CFLAGS[@]}" \
  CPPFLAGS="${XAR_CFLAGS[@]}" \
  LDFLAGS="${XAR_LDFLAGS[@]}" \
  ./configure \
    --prefix= \
    --enable-static \
    --disable-shared \
    --with-lzma="$XZ" \
    --with-xml2-config=/usr/bin/xml2-config \
    --without-bzip2

  make -j$(nproc) lib_static

  mkdir -p "$LIBXAR/lib" "$LIBXAR/include/xar"
  install -vm 0644 include/xar.h "$LIBXAR/include/xar/xar.h"
  install -vm 0644 lib/libxar.a "$LIBXAR/lib/libxar.a"

  _popd
fi

# ————————————————————————————————————————————————————————————————————————————————————
# pbzx

echo "building $(_relpath "$PBZX")"
mkdir -p "$(dirname "$PBZX")"
"$OUT_DIR/opt/co" cc \
  "-I$LLVMBOX/include" "-L$LLVMBOX/lib" -lz \
  "-I$XZ/include"      "-L$XZ/lib"      -llzma \
  "-I$OPENSSL/include" "-L$OPENSSL/lib" -lcrypto \
  "-I$LIBXAR/include"  "-L$LIBXAR/lib"  -lxar \
  $(/usr/bin/xml2-config --cflags --libs) \
  "$PBZX_SRC" -o "$PBZX"
