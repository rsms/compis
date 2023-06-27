// SPDX-License-Identifier: Apache-2.0
#include "colib.h"
#include <string.h>


isize slastindexofn(const char* s, usize len, char c) {
  if UNLIKELY(len > ISIZE_MAX)
    len = ISIZE_MAX;
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
  return slastindexofn(s, strlen(s), c);
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
