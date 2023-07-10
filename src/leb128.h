// SPDX-License-Identifier: Apache-2.0
#pragma once
ASSUME_NONNULL_BEGIN

// LEB128: Little Endian Base 128
#define LEB128_NBYTE_64 10 // bytes needed to represent all 64-bit integer values
#define LEB128_NBYTE_32 5  // bytes needed to represent all 32-bit integer values
u32 leb128_size(u64 val); // actual bytes needed
u32 leb128_u64_write(u8 out[LEB128_NBYTE_64], u64 val);
u32 leb128_u32_write(u8 out[LEB128_NBYTE_32], u32 val);

ASSUME_NONNULL_END
