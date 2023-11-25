// SPDX-License-Identifier: Apache-2.0
#include "colib.h"
#include "unicode.h"

// lookup tables used by utf8_decode
//
// first_byte_mark
//   Once the bits are split out into bytes of UTF-8, this is a mask OR-ed into the
//   first byte, depending on how many bytes follow. There are as many entries in this
//   table as there are UTF-8 sequence types. (I.e., one byte sequence, two byte... etc.).
//   Remember that sequencs for *legal* UTF-8 will be 4 or fewer bytes total.
static const u8 first_byte_mark[7] = { 0x00, 0x00, 0xC0, 0xE0, 0xF0, 0xF8, 0xFC };
//
static const u8 utf8_seqlentab[] = {
      2,2,2,2,2,2,2,2,2,2,2,2,2,2, /* 0xC2-0xCF */
  2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2, /* 0xD0-0xDF */
  3,3,3,3,3,3,3,3,3,3,3,3,3,3,3,3, /* 0xE0-0xEF */
  4,4,4,4,4                        /* 0xF0-0xF4 */
};
//
static const u32 dec_mintab[] = {4194304, 0, 128, 2048, 65536};
static const u8 dec_shiftetab[] = {0, 6, 4, 2, 0};


bool rune_isvalid(rune_t r) {
  // The range from U+D800 to U+DFFF is reserved for surrogate pairs
  // in UTF-16 encoding and is not valid for a standalone Unicode codepoint
  return (r < 0xD800) || (r >= 0xE000 && r <= RUNE_MAX);
}


rune_t utf8_decode(const u8** src, const u8* end) {
  assertf(*src != end, "empty input");

  const u8* p = *src;
  const u8* s = p;
  u8 b0 = *p;

  if (b0 < 0xC2 || b0 > 0xF4) {
    (*src)++;
    return (b0 < RUNE_SELF) ? b0 : RUNE_INVALID;
  }

  u8 len = utf8_seqlentab[b0 - 0xC2];
  *src += len;

  if UNLIKELY(*src > end) {
    *src = end;
    return false;
  }

  rune_t r = 0;
  switch (len) {
    case 4: r += *p++; r <<= 6; FALLTHROUGH;
    case 3: r += *p++; r <<= 6;
  }
  r += *p++; r <<= 6;
  r += *p++;

  // precomputed values to subtract from codepoint, depending on how many shifts we did
  static const rune_t subtab[] = {0,0x3080,0xE2080,0x3C82080};
  r -= subtab[len - 1];

  // accumulate error conditions
  int e = (r < dec_mintab[len]) << 6; // non-canonical encoding
  e |= ((r >> 11) == 0x1b) << 7; // surrogate half?
  e |= (r > RUNE_MAX) << 8; // out of range?
  e |= (s[1] & 0xc0) >> 2;
  if (len > 2) e |= (s[2] & 0xc0) >> 4;
  if (len > 3) e |= (s[3]       ) >> 6;
  e ^= 0x2a; // top two bits of each tail byte correct?
  e >>= dec_shiftetab[len];

  return e ? RUNE_INVALID : r;
}


bool utf8_encode(u8** dst, const u8* dstend, rune_t r) {
  bool ok = true;
  usize n = 4;
  if      (r < 0x80)      n = 1;
  else if (r < 0x800)     n = 2;
  else if (r < 0x10000) { n = 3; ok = (r < 0xD800 || r > 0xDFFF); }
  else if UNLIKELY(r > RUNE_MAX) { r = RUNE_SUB; n = 3; ok = false; }

  u8* p = *dst + n;
  if UNLIKELY(p > dstend)
    return false;
  *dst = p;

  switch (n) {
    case 4: *--p = (u8)((r | (rune_t)0x80) & (rune_t)0xBF); r >>= 6; FALLTHROUGH;
    case 3: *--p = (u8)((r | (rune_t)0x80) & (rune_t)0xBF); r >>= 6; FALLTHROUGH;
    case 2: *--p = (u8)((r | (rune_t)0x80) & (rune_t)0xBF); r >>= 6; FALLTHROUGH;
    case 1: *--p = (u8) (r | first_byte_mark[n]);
  }

  return ok;
}
