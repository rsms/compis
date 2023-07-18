// SPDX-License-Identifier: Apache-2.0
#include "colib.h"
#include "buf.h"
#include "leb128.h"


// buf_t should be castable to mem_t
static_assert(offsetof(buf_t,p) == offsetof(mem_t,p), "");
static_assert(offsetof(buf_t,cap) == offsetof(mem_t,size), "");

// slice_t should be castable to mem_t
static_assert(offsetof(slice_t,p) == offsetof(mem_t,p), "");
static_assert(offsetof(slice_t,len) == offsetof(mem_t,size), "");


static const char* kHexchars = "0123456789abcdef";


void buf_init(buf_t* b, memalloc_t ma) {
  b->p = NULL;
  b->cap = 0;
  b->len = 0;
  b->ma = ma;
  b->external = false;
}


void buf_initext(buf_t* b, memalloc_t ma, void* p, usize cap) {
  b->p = p;
  b->cap = cap;
  b->len = 0;
  b->ma = ma;
  b->external = true;
}


void buf_dispose(buf_t* b) {
  if (!b->external && b->cap)
    mem_free(b->ma, (mem_t*)b);
  #ifdef CO_SAFE
  memset(b, 0, sizeof(*b));
  #endif
}


bool buf_grow(buf_t* b, usize extracap) {
  usize newcap;
  if (b->cap == 0) {
    newcap = MAX(256, CEIL_POW2(extracap));
  } else if (b->cap < extracap || check_mul_overflow(b->cap, 2lu, &newcap)) {
    // either the current capacity is less than what we need or cap>USIZE_MAX/2
    if (check_add_overflow(b->cap, CEIL_POW2(extracap), &newcap)) {
      // fall back to exact growth
      if (check_add_overflow(b->cap, extracap, &newcap))
        return false;
    }
  }
  if (!b->external)
    return mem_resize(b->ma, (mem_t*)b, newcap);
  mem_t m = mem_alloc(b->ma, newcap);
  if (!m.p)
    return false;
  memcpy(m.p, b->p, b->len);
  b->p = m.p;
  b->cap = newcap;
  b->external = false;
  return true;
}


bool buf_reserve(buf_t* b, usize minavail) {
  if (buf_avail(b) >= minavail)
    return true;
  usize newlen;
  if (check_add_overflow(b->len, minavail, &newlen))
    return false;
  return buf_grow(b, newlen - b->cap);
}


bool buf_nullterm(buf_t* b) {
  if (UNLIKELY(b->len >= b->cap) && UNLIKELY(!buf_grow(b, 1)))
    return false;
  b->bytes[b->len] = 0;
  return true;
}


void* nullable buf_alloc(buf_t* b, usize len) {
  usize newlen;
  if (len == 0 || check_add_overflow(b->len, len, &newlen))
    return NULL;
  if (newlen > b->cap && UNLIKELY(!buf_grow(b, newlen - b->cap)))
    return NULL;
  u8* p = b->bytes + b->len;
  b->len = newlen;
  return p;
}


bool buf_append(buf_t* b, const void* src, usize len) {
  if (len == 0)
    return true;
  void* p = buf_alloc(b, len);
  if (p)
    memcpy(p, src, len);
  return !!p;
}


bool buf_appendrepr(buf_t* b, const void* src, usize len) {
  if (len == 0)
    return true;

  // estimate size needed to 150% of input size
  usize cap; // cap = floor(len * 1.5)
  if (check_mul_overflow(len, (usize)15, &cap)) {
    cap = len;
  } else {
    cap = (cap / 10) + 1; // +1 for null terminator, needed by string_repr
  }

  usize n;
  for (;;) {
    if UNLIKELY(!buf_reserve(b, cap))
      return false;
    n = string_repr(b->p + b->len, b->cap - b->len, src, len);
    if (n < cap)
      break;
    cap = n + 1;
  }

  // note: string_repr writes a terminating NUL char
  if (check_add_overflow(b->len, n, &n)) {
    return false;
  }
  b->len = n;
  return true;
}


bool buf_appendhex(buf_t* b, const void* src, usize len) {
  if (len == 0)
    return true;

  usize nwrite = len * 2;
  if UNLIKELY(!buf_reserve(b, nwrite))
    return false;

  char* p = b->chars + b->len;
  const u8* srcp = src;

  for (usize i = 0; i < len; i++) {
    u8 c = *srcp++;
    if (c < 0x10) {
      p[0] = '0';
      p[1] = kHexchars[c];
    } else {
      p[0] = kHexchars[c >> 4];
      p[1] = kHexchars[c & 0xf];
    }
    p += 2;
  }

  b->len += nwrite;
  return true;
}


bool buf_insert(buf_t* b, usize index, const void* src, usize len) {
  assertf(index <= b->len, "index(%zu) > buf.len(%zu)", index, b->len);
  if (len == 0)
    return true;
  if UNLIKELY(!buf_reserve(b, len))
    return false;
  void* dst = &b->bytes[index];
  if (index < b->len)
    memmove(&b->bytes[index + len], dst, b->len - index);
  memcpy(dst, src, len);
  b->len += len;
  return true;
}


bool buf_fill(buf_t* b, u8 byte, usize len) {
  void* p = buf_alloc(b, len);
  if (p)
    memset(p, byte, len);
  return !!p;
}


bool buf_vprintf(buf_t* b, const char* fmt, va_list ap) {
  va_list ap2;
  usize needavail = strlen(fmt)*2;
  if (needavail == 0)
    return true;
  for (;;) {
    if (needavail > INT_MAX || !buf_reserve(b, needavail))
      return false;
    va_copy(ap2, ap); // copy va_list since we might read it twice
    int n = vsnprintf(&b->p[b->len], buf_avail(b), fmt, ap2);
    va_end(ap2);
    if (n < (int)needavail) {
      b->len += (usize)n;
      return true;
    }
    needavail = (usize)n + 1;
  }
}


bool buf_printf(buf_t* b, const char* fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  bool ok = buf_vprintf(b, fmt, ap);
  va_end(ap);
  return ok;
}


bool buf_print_u64(buf_t* b, u64 n, u32 base) {
  char buf[64];
  u32 len = (u32)sfmtu64(buf, n, base);
  return buf_append(b, buf, len);
}


bool buf_print_u32(buf_t* b, u32 n, u32 base) {
  if UNLIKELY(!buf_reserve(b, 32))
    return false;
  b->len += sfmtu64(b->chars + b->len, (u64)n, base);
  return true;
}


bool buf_print_leb128_u32(buf_t* b, u32 n) {
  if (!buf_reserve(b, LEB128_NBYTE_32))
    return false;
  b->len += leb128_u32_write(b->bytes + b->len, n);
  return true;
}


bool buf_print_leb128_u64(buf_t* b, u64 n) {
  if (!buf_reserve(b, LEB128_NBYTE_64))
    return false;
  b->len += leb128_u64_write(b->bytes + b->len, n);
  return true;
}
