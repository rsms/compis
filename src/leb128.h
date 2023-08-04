// SPDX-License-Identifier: Apache-2.0
#pragma once
ASSUME_NONNULL_BEGIN

// LEB128: Little Endian Base 128
#define LEB128_NBYTE_64 10 // bytes needed to represent U64_MAX
#define LEB128_NBYTE_32 5  // bytes needed to represent U32_MAX
u32 leb128_size(u64 val);  // actual bytes needed
u32 leb128_u64_write(u8 out[LEB128_NBYTE_64], u64 val);
u32 leb128_u32_write(u8 out[LEB128_NBYTE_32], u32 val);

// returns number of bytes read from input, or err_t (negative value) on error.
// nbit is used for overflow checking.
int leb128_read(u64* resultp, u32 nbit, const u8* in, const void* inend);

ASSUME_NONNULL_END
