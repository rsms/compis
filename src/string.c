// SPDX-License-Identifier: Apache-2.0
#include "colib.h"
#include <string.h>


const u8 g_intdectab[256] = { // base 2-36
  -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
  -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
  -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
   0, 1, 2, 3, 4, 5, 6, 7, 8, 9,-1,-1,-1,-1,-1,-1, // 0-9
  -1,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24, // A-Z
  25,26,27,28,29,30,31,32,33,34,35,-1,-1,-1,-1,-1,
  -1,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24, // a-z
  25,26,27,28,29,30,31,32,33,34,35,-1,-1,-1,-1,-1,
  -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
  -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
  -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
  -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
  -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
  -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
  -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
  -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
};


static const char kEncChars[] = {
  "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz"
};


isize string_lastindexof(const char* s, usize len, char c) {
  if (len > (usize)ISIZE_MAX)
    len = (usize)ISIZE_MAX;
  while (len--) {
    if (s[len] == c)
      return (isize)len;
  }
  return -1;
}


isize sindexof(const char* s, char c) {
  char* p = strchr(s, c);
  return p ? (isize)(uintptr)(p - s) : -1;
}


isize slastindexof(const char* s, char c) {
  return string_lastindexof(s, strlen(s), c);
}


isize string_indexof(const char* p, usize len, char c) {
  const char* pp = memchr(p, (int)c, len);
  return pp ? (isize)(uintptr)(pp - p) : -1l;
}

isize string_indexofstr(
  const char* haystack, usize haystack_len,
  const char* needle, usize needle_len)
{
  // slow, naive implementation
  if (haystack_len == 0 || needle_len == 0 || haystack_len < needle_len)
    return -1;
  for (usize i = 0; i <= haystack_len - needle_len; i++) {
    usize j;
    for (j = 0; j < needle_len; j++) {
      if (haystack[i + j] != needle[j])
        break;
    }
    if (j == needle_len)
      return (isize)i;
  }
  return -1;
}


const char* strim_begin(const char* s, usize len, char trimc) {
  usize i = 0;
  for (; s[i] == trimc && i < len; i++) {
  }
  return s + i;
}


usize strim_end(const char* s, usize len, char trimc) {
  for (; len && s[len - 1] == trimc; len--) {
  }
  return len;
}


static char* strrevn(char* s, usize len) {
  for (usize i = 0, j = len - 1; i < j; i++, j--) {
    char tmp = s[i];
    s[i] = s[j];
    s[j] = tmp;
  }
  return s;
}


u32 ndigits10(u64 v) {
  // based on https://www.facebook.com/notes/10158791579037200/
  if (v < 10) return 1;
  if (v < 100) return 2;
  if (v < 1000) return 3;
  if (v < 1000000000000UL) {
    if (v < 100000000UL) {
      if (v < 1000000) {
        if (v < 10000) return 4;
        return 5 + (v >= 100000);
      }
      return 7 + (v >= 10000000UL);
    }
    if (v < 10000000000UL)
      return 9 + (v >= 1000000000UL);
    return 11 + (v >= 100000000000UL);
  }
  return 12 + ndigits10(v / 1000000000000UL);
}


u32 sndigits10(i64 v) {
  if (v < 0) {
    u64 uv = (v != I64_MIN) ? (u64)-v : ((u64)I64_MAX) + 1;
    return ndigits10(uv) + 1; // +1 for '-'
  }
  return ndigits10((u64)v);
}


u32 ndigits16(u64 v) {
  if (v < 0x10) return 1;
  if (v < 0x100) return 2;
  if (v < 0x1000) return 3;
  if (v < 0x1000000000000UL) {
    if (v < 0x100000000UL) {
      if (v < 0x1000000) {
        if (v < 0x10000) return 4;
        return 5 + (v >= 0x100000);
      }
      return 7 + (v >= 0x10000000UL);
    }
    if (v < 0x10000000000UL)
      return 9 + (v >= 0x1000000000UL);
    return 11 + (v >= 0x100000000000UL);
  }
  return 12 + ndigits10(v / 0x1000000000000UL);
}


u32 fmt_u64_base10(char* dst, usize cap, u64 value) {
  // based on https://www.facebook.com/notes/10158791579037200/
  static const char digits[201] =
    "0001020304050607080910111213141516171819"
    "2021222324252627282930313233343536373839"
    "4041424344454647484950515253545556575859"
    "6061626364656667686970717273747576777879"
    "8081828384858687888990919293949596979899";

  u32 len = ndigits10(value);
  if ((usize)len > cap)
    return 0;

  u32 next = len - 1;
  // dst[next + 1] = '\0';
  while (value >= 100) {
    u32 i = (u32)(value % 100ul) * 2;
    value /= 100ull;
    dst[next] = digits[i + 1];
    dst[next - 1] = digits[i];
    next -= 2;
  }

  // handle last 1-2 digits
  if (value < 10) {
    dst[next] = '0' + (u32)value;
  } else {
    u32 i = (u32)value * 2u;
    dst[next] = digits[i + 1];
    dst[next - 1] = digits[i];
  }

  return len;
}


u32 fmt_i64_base10(char* dst, usize cap, i64 svalue) {
  u64 value;
  u32 isneg = 0;

  // convert to u64
  if (svalue < 0) {
    if (svalue != I64_MIN) {
      value = -svalue;
    } else {
      value = ((u64)I64_MAX) + 1;
    }
    if (cap == 0)
      return 0;
    isneg = 1;
    dst[0] = '-';
    dst++;
    cap--;
  } else {
    value = svalue;
  }

  u32 len = fmt_u64_base10(dst, cap, value);
  if (len == 0)
    return 0;
  return len + isneg;
}


usize sfmtu64(char* buf, u64 v, u32 base) {
  base = MIN(MAX(base, 2u), 62u);
  char* p = buf;
  do {
    *p++ = kEncChars[v % base];
    v /= base;
  } while (v);
  usize len = (usize)(uintptr)(p - buf);
  p--;
  strrevn(buf, len);
  return len;
}


u32 fmt_u64_base16(char* dst, usize cap, u64 value) {
  if (cap < ndigits16(value))
    return 0;
  return (u32)sfmtu64(dst, value, 16);
}


u32 fmt_u64_base62(char dst[11], usize cap, u64 val) {
  // 0xffffffffffffffff => "LygHa16AHYF"
  char* p = dst;
  u64 rem;
  if UNLIKELY(cap < 11) {
    if (val == 0) return 1;
    for (;;) {
      rem = val % 62;
      val = val / 62;
      p++;
      if (val == 0)
        break;
    }
  } else {
    for (;;) {
      rem = val % 62;
      val = val / 62;
      *p++ = kEncChars[rem];
      if (val == 0)
        break;
    }
  }
  return (u32)(uintptr)(p - dst);
}


bool string_startswith(const char* s, const char* prefix) {
  usize slen = strlen(s);
  usize prefixlen = strlen(prefix);
  return slen >= prefixlen && memcmp(s, prefix, prefixlen) == 0;
}


bool string_endswithn(const char* s, usize slen, const char* suffix, usize suffixlen) {
  return slen >= suffixlen && memcmp(s + (slen - suffixlen), suffix, suffixlen) == 0;
}


bool str_endswith(const char* s, const char* suffix) {
  return string_endswithn(s, strlen(s), suffix, strlen(suffix));
}


char* _strcat(char* buf, usize bufcap, usize count, ...) {
  va_list ap;
  char* p = buf;
  assert(bufcap > 0);
  va_start(ap, count);
  while (count--) {
    const char* s = va_arg(ap, const char*);
    usize len = va_arg(ap, usize);
    assert(p + len < buf + bufcap);
    memcpy(p, s, len);
    p += len;
  }
  va_end(ap);
  *p = 0;
  return buf;
}


static const char* hexchars = "0123456789abcdef";

usize string_repr(char* dst, usize dstcap, const void* srcp, usize len) {
  char* p;
  char* lastp;
  char tmpc;
  usize nwrite = 0;

  if (dstcap == 0) {
    p = &tmpc;
    lastp = &tmpc;
  } else {
    p = dst;
    lastp = dst + (dstcap - 1);
  };

  for (usize i = 0; i < len; i++) {
    u8 c = *(u8*)srcp++;
    //dlog("[%zu/%zu] 0x%02x '%c'", i, len, c, isprint(c) ? c : ' ');
    switch (c) {
      // \xHH
      case '\1'...'\x08':
      case 0x0E ... 0x1F:
      case 0x7f ... 0xFF:
        if (LIKELY( p + 3 < lastp )) {
          p[0] = '\\';
          p[1] = 'x';
          if (c < 0x10) {
            p[2] = '0';
            p[3] = hexchars[(int)c];
          } else {
            p[2] = hexchars[(int)c >> 4];
            p[3] = hexchars[(int)c & 0xf];
          }
          p += 4;
        } else {
          p = lastp;
        }
        nwrite += 4;
        break;
      // \c
      case '\t'...'\x0D':
      case '\\':
      case '"':
      case '\0': {
        static const char t[] = {'t','n','v','f','r'};
        if (LIKELY( p + 1 < lastp )) {
          p[0] = '\\';
          if      (c == 0)                         p[1] = '0';
          else if (((usize)c - '\t') <= sizeof(t)) p[1] = t[c - '\t'];
          else                                     p[1] = c;
          p += 2;
        } else {
          p = lastp;
        }
        nwrite += 2;
        break;
      }
      // verbatim
      default:
        *p = c;
        p = MIN(p + 1, lastp);
        nwrite++;
        break;
    }
  }

  *p = 0;
  return nwrite;
}
