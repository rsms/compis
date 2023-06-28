// hash functions and PRNG
// SPDX-License-Identifier: Apache-2.0
#pragma once
ASSUME_NONNULL_BEGIN

// wyhash computes a good hash for arbitrary bytes.
// https://github.com/wangyi-fudan/wyhash
u64 wyhash(const void* key, usize len, u64 seed, const u64 secret[4]);

// wyhash64 is a useful 64bit-64bit mix function to produce deterministic
// pseudo random numbers that can pass BigCrush and PractRand
u64 wyhash64(u64 a, u64 b);

// fastrand returns the next pseudo-random number
u64 fastrand();

ASSUME_NONNULL_END
