#!/usr/bin/env bash
set -euo pipefail
source "$(dirname "$0")/lib.sh"
cd "$PROJECT"

VERSION=1.0.2
WORKDIR=$DEPS_DIR/wasi.js-$VERSION
OUTFILE=$PROJECT/examples/hello-wasi/wasi.js

rm -rf "$WORKDIR"
mkdir -p "$WORKDIR"
_pushd "$WORKDIR"

npm install \
  @wasmer/wasi \
  @wasmer/wasmfs \
  esbuild \
  node-stdlib-browser

./node_modules/.bin/esbuild \
  --bundle \
  --minify \
  --target=chrome90,firefox76,safari14 \
  --format=esm \
  --loader:.wasm=binary \
  --inject:node_modules/node-stdlib-browser/helpers/esbuild/shim.js \
  --define:Buffer=Buffer \
  --outfile="$OUTFILE" \
  --banner:js="// SPDX-License-Identifier: MIT
// https://github.com/wasmerio/wasmer-js" \
  node_modules/@wasmer/wasi/dist/Library.esm.js
