// SPDX-License-Identifier: Apache-2.0
#include "colib.h"
#include "leb128.h"


// // returns number of bytes read from input, or err_t (negative value) on error
// int leb128_read(u64* resultp, u32 nbit, const u8* in, const void* inend) {
//   assert(nbit > 0 && nbit <= 64 && IS_POW2(nbit));
//   u64 v = 0;
//   u32 shift = 0;
//   if (in == inend)
//     return ErrInvalid;
//   const u8* p = in;
//   while (p != inend) {
//     u64 b = *p++;
//     v |= ((b & 0x7f) << shift);
//     shift += 7;
//     if ((b & 0x80) == 0)
//       break;
//     if (shift >= nbit)
//       return ErrOverflow;
//   }
//   *resultp = v;
//   return (int)(uintptr)(p - in);
// }


#define _LEB128_MORE_S(T) \
  ( (tmp != 0 && tmp != (T)-1) || (val >= 0 && (byte&64)) || (val < 0 && (byte&64)==0) )
#define _LEB128_MORE_U(T) \
  (tmp != 0)
#define _LEB128_DEF_WRITE(NAME, T, NBYTE, MORE) \
  static u32 NAME(u8 out[NBYTE], T val) {  \
    T tmp = val; bool more; u32 i = 0;     \
    do {                                     \
      u8 byte = tmp & 127;                   \
      tmp >>= 7;                             \
      more = MORE(T);                        \
      if (more)                              \
        byte = byte | 128;                   \
      out[i++] = byte;                       \
    } while (more);                          \
    return i;                                \
  }


// u32 leb128_u64_write(u8 out[LEB128_NBYTE_64], u64 val);
// u32 leb128_u32_write(u8 out[LEB128_NBYTE_32], u32 val);
// u32 leb128_i64_write(u8 out[LEB128_NBYTE_64], i64 val);
// u32 leb128_i32_write(u8 out[LEB128_NBYTE_32], i32 val);
_LEB128_DEF_WRITE(leb128_u64_write, u64, LEB128_NBYTE_64, _LEB128_MORE_U)
_LEB128_DEF_WRITE(leb128_u32_write, u32, LEB128_NBYTE_32, _LEB128_MORE_U)
// _LEB128_DEF_WRITE(leb128_i64_write, i64, LEB128_NBYTE_64, _LEB128_MORE_S)
// _LEB128_DEF_WRITE(leb128_i32_write, i32, LEB128_NBYTE_32, _LEB128_MORE_S)

u32 leb128_size(u64 val) {
  for (u32 len = 0;;) {
    val >>= 7;
    len++;
    if (val == 0)
      return len;
  }
}
