#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."

rm -rf lib/sysroot/*
mkdir -p lib/sysroot
cp -vPR deps/llvmbox/targets/* lib/sysroot/
