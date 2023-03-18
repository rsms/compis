// SPDX-License-Identifier: Apache-2.0
//
// If you update these definitions:
// - also update target_default in target.c
// - inspect _co_targets function in etc/lib.sh and update if needed
//   (it greps for content in this file)
//
#ifndef TARGET
  #define TARGET(...)
#endif
#ifndef ARCH
  #define ARCH(...)
#endif
#ifndef SYS
  #define SYS(...)
#endif

// ARCH(name)
ARCH(any)
ARCH(aarch64)
ARCH(arm)
ARCH(i386)
ARCH(riscv64)  // no riscv32 since musl doesn't (yet) support it
ARCH(wasm32)
ARCH(wasm64)
ARCH(x86_64)

// SYS(name)
SYS(none)
SYS(macos)
SYS(linux)
SYS(wasi)

// TARGET(arch, sys, sysver, intsize, ptrsize, llvm_triple)
// IMPORTANT: these MUST be sorted by sysver, per sys (for default min version select)
TARGET(aarch64, linux, "", 8, 8, "aarch64-linux-musl")
TARGET(arm,     linux, "", 4, 4, "arm-linux-musl")
TARGET(i386,    linux, "", 4, 4, "i386-linux-musl")
TARGET(riscv64, linux, "", 8, 8, "riscv64-linux-musl")
TARGET(x86_64,  linux, "", 8, 8, "x86_64-linux-musl")

TARGET(aarch64, macos, "11", 8, 8, "arm64-apple-macosx11.0.0")
TARGET(aarch64, macos, "12", 8, 8, "arm64-apple-macosx12.0.0")
TARGET(aarch64, macos, "13", 8, 8, "arm64-apple-macosx13.0.0")

TARGET(x86_64,  macos, "10", 8, 8, "x86_64-apple-macosx10.15.0")
TARGET(x86_64,  macos, "11", 8, 8, "x86_64-apple-macosx11.0.0")
TARGET(x86_64,  macos, "12", 8, 8, "x86_64-apple-macosx12.0.0")
TARGET(x86_64,  macos, "13", 8, 8, "x86_64-apple-macosx13.0.0")

TARGET(wasm32,  wasi, "", 4, 4, "wasm32-wasi")
TARGET(wasm32,  none, "", 4, 4, "wasm32-unknown-unknown")
TARGET(wasm64,  none, "", 4, 4, "wasm64-unknown-unknown")
