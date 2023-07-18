/*
intscan from musl adapted to compis.
musl is licensed under the following standard MIT license:

Copyright © 2005-2020 Rich Felker, et al.

Permission is hereby granted, free of charge, to any person obtaining
a copy of this software and associated documentation files (the
"Software"), to deal in the Software without restriction, including
without limitation the rights to use, copy, modify, merge, publish,
distribute, sublicense, and/or sell copies of the Software, and to
permit persons to whom the Software is furnished to do so, subject to
the following conditions:

The above copyright notice and this permission notice shall be
included in all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
*/
#include "colib.h"

#define NEXTCH()  ( ((src) < srcend) ? *(src)++ : ((u8)-1) )  // shgetc


err_t co_intscan(const u8** srcp, usize srclen, u32 base, u64 lim, u64* result) {
  const u8* val = g_intdectab;
  const u8* src = *srcp;
  const u8* srcend = src + srclen;
  err_t err = 0;
  u8 c;
  int neg = 0;
  u32 x;
  u64 y;

  if (srclen == 0)
    return ErrEnd;

  if (base > 36 || base == 1)
    return ErrInvalid;

  c = *src++; // while (isspace((c = NEXTCH()))) {}

  if (c=='+' || c=='-') {
    neg = -(c=='-');
    c = NEXTCH();
  }

  // handle "0x" prefix
  if ((base == 0 || base == 16) && c == '0') {
    c = NEXTCH();
    if ((c | 32) == 'x') { // "0x" or "0X"
      c = NEXTCH();
      if UNLIKELY(val[c] >= 16) {
        // invalid first digit or premature end of input, e.g. "0xK" or "0x"
        *srcp = src;
        return ErrInvalid;
      }
      base = 16;
    } else if (base == 0) {
      // note: musl sets base to 8 in this case to support octal e.g. "0744",
      // but we don't support that so default base to 10, i.e. "012" == 12.
      base = 10;
    }
  } else {
    // base defaults to 10
    if (base == 0)
      base = 10;
    if UNLIKELY(val[c] >= base) {
      *srcp = src;
      return ErrInvalid;
    }
  }

  if (base == 10) {
    for (x=0; c-'0' < 10U && x<=UINT_MAX/10-1; c=NEXTCH())
      x = x*10 + (c-'0');
    for (y=x; c-'0'<10U && y<=ULLONG_MAX/10 && 10*y<=ULLONG_MAX-(c-'0'); c=NEXTCH())
      y = y*10 + (c-'0');
    if (c-'0'>=10U)
      goto done;
  } else if (!(base & base-1)) {
    int bs = "\0\1\2\4\7\3\6\5"[(0x17*base)>>5&7];
    for (x=0; val[c]<base && x<=UINT_MAX/32; c=NEXTCH())
      x = x<<bs | val[c];
    for (y=x; val[c]<base && y<=ULLONG_MAX>>bs; c=NEXTCH())
      y = y<<bs | val[c];
  } else {
    for (x=0; val[c]<base && x<=UINT_MAX/36-1; c=NEXTCH())
      x = x*base + val[c];
    for (
      y = x;
      val[c]<base && y <= ULLONG_MAX/base && base*y <= ULLONG_MAX-val[c];
      c = NEXTCH() )
    {
      y = y*base + val[c];
    }
  }

  if (val[c] < base) {
    for (; val[c] < base; c = NEXTCH());
    err = ErrOverflow;
    y = lim;
    if (lim&1)
      neg = 0;
  }

done:
  src--;

  if (y >= lim) {
    if (!(lim & 1) && !neg) {
      *result = lim - 1;
      err = ErrOverflow;
    } else if (y>lim) {
      *result = lim;
      err = ErrOverflow;
    }
  }

  *result = (y ^ neg) - neg;
  *srcp = src;
  return err;
}
