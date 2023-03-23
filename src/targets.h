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
ARCH(arm)  // ARMv6, float-abi=hard  FIXME
ARCH(i386)
ARCH(riscv64)
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

TARGET(aarch64, macos, "11", 8, 8, "arm64-apple-darwin20")
TARGET(aarch64, macos, "12", 8, 8, "arm64-apple-darwin21")
TARGET(aarch64, macos, "13", 8, 8, "arm64-apple-darwin22")

TARGET(x86_64,  macos, "10", 8, 8, "x86_64-apple-darwin19")
TARGET(x86_64,  macos, "11", 8, 8, "x86_64-apple-darwin20")
TARGET(x86_64,  macos, "12", 8, 8, "x86_64-apple-darwin21")
TARGET(x86_64,  macos, "13", 8, 8, "x86_64-apple-darwin22")

TARGET(wasm32, wasi, "", 4, 4, "wasm32-wasi")

TARGET(aarch64, none, "", 8, 8, "aarch64-unknown-unknown")
TARGET(arm,     none, "", 4, 4, "arm-unknown-unknown")
TARGET(i386,    none, "", 4, 4, "i386-unknown-unknown")
TARGET(riscv64, none, "", 8, 8, "riscv64-unknown-unknown")
TARGET(wasm32,  none, "", 4, 4, "wasm32-unknown-unknown")
TARGET(wasm64,  none, "", 4, 4, "wasm64-unknown-unknown")
TARGET(x86_64,  none, "", 8, 8, "x86_64-unknown-unknown")


// arm Raspberry Pi targets:
//
//   Zero/W/WH & 1 Model A/B/A+/B+
//     -march=armv6 -mfloat-abi=hard -mfpu=vfp
//
//   2 & 3 Model A/B
//     -march=armv7-a -mfloat-abi=hard -mfpu=neon-vfpv4
//
//   3 & 4 Model A+/B+ & Compute 3/3-lite/3+ (32-Bit)
//     -march=armv8-a -mfloat-abi=hard -mfpu=neon-fp-armv8
//
//   3 & 4 Model A+/B+ & Compute 3/3-lite/3+ (64-Bit)
//     -march=armv8-a+fp+simd
//
