// SPDX-License-Identifier: Apache-2.0
#include "colib.h"
#include <string.h>


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


usize sfmtu64(char* buf, u64 v, u32 base) {
  static const char* chars =
    "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz";
  base = MIN(MAX(base, 2u), 62u);
  char* p = buf;
  do {
    *p++ = chars[v % base];
    v /= base;
  } while (v);
  usize len = (usize)(uintptr)(p - buf);
  p--;
  strrevn(buf, len);
  return len;
}


bool string_startswith(const char* s, const char* prefix) {
  usize slen = strlen(s);
  usize prefixlen = strlen(prefix);
  return slen >= prefixlen && memcmp(s, prefix, prefixlen) == 0;
}


bool str_endswith(const char* s, const char* suffix) {
  usize slen = strlen(s);
  usize suffixlen = strlen(suffix);
  return slen >= suffixlen && memcmp(s + (slen - suffixlen), suffix, suffixlen) == 0;
}


int u64log10(u64 u) {
  // U64_MAX 18446744073709551615
  int w = 20;
  u64 x = 10000000000000000000llu;
  while (w > 1) {
    if (u >= x)
      break;
    x /= 10;
    w--;
  }
  return w;
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
