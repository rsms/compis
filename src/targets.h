// SPDX-License-Identifier: Apache-2.0
//
// If you update these definitions, also update target_default in target.c
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
ARCH(aarch64)
ARCH(arm)
ARCH(i386)
ARCH(riscv32)
ARCH(riscv64)
ARCH(wasm32)
ARCH(wasm64)
ARCH(x86_64)

// SYS(name)
SYS(macos)
SYS(linux)

// TARGET(arch, sys, sysver, intsize, ptrsize, llvm_triple)
// IMPORTANT: these MUST be sorted by sysver, per sys (for default min version select)
TARGET(aarch64, linux, "", 8, 8, "aarch64-linux-musl")
TARGET(arm,     linux, "", 4, 4, "arm-linux-musl")
TARGET(i386,    linux, "", 4, 4, "i386-linux-musl")
TARGET(riscv32, linux, "", 4, 4, "riscv32-linux-musl")
TARGET(riscv64, linux, "", 8, 8, "riscv64-linux-musl")
TARGET(x86_64,  linux, "", 8, 8, "x86_64-linux-musl")

TARGET(aarch64, macos, "11", 8, 8, "arm64-apple-darwin20")
TARGET(aarch64, macos, "12", 8, 8, "arm64-apple-darwin21")
TARGET(aarch64, macos, "13", 8, 8, "arm64-apple-darwin22")

TARGET(x86_64,  macos, "10", 8, 8, "x86_64-apple-darwin19")
TARGET(x86_64,  macos, "11", 8, 8, "x86_64-apple-darwin20")
TARGET(x86_64,  macos, "12", 8, 8, "x86_64-apple-darwin21")
TARGET(x86_64,  macos, "13", 8, 8, "x86_64-apple-darwin22")

