#include "c0lib.h"
#include "buf.h"


// buf_t should be castable to mem_t
static_assert(offsetof(buf_t,p) == offsetof(mem_t,p), "");
static_assert(offsetof(buf_t,cap) == offsetof(mem_t,size), "");

// slice_t should be castable to mem_t
static_assert(offsetof(slice_t,p) == offsetof(mem_t,p), "");
static_assert(offsetof(slice_t,len) == offsetof(mem_t,size), "");


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
  if (!b->external)
    mem_free(b->ma, (mem_t*)b);
  b->len = 0;
}


bool buf_grow(buf_t* b, usize extracap) {
  usize newcap;
  if (b->cap == 0) {
    newcap = MAX(256, CEIL_POW2(extracap));
  } else {
    if (check_mul_overflow(b->cap, (usize)2, &newcap))
      if (check_add_overflow(b->cap, extracap, &newcap))
        return false;
  }
  if (b->external) {
    mem_t m = mem_alloc(b->ma, newcap);
    if (!m.p)
      return false;
    memcpy(m.p, b->p, b->len);
    b->p = m.p;
    b->cap = newcap;
    b->external = false;
    return true;
  }
  return mem_resize(b->ma, (mem_t*)b, newcap);
}


bool buf_reserve(buf_t* b, usize minavail) {
  usize newlen;
  if (check_add_overflow(b->len, minavail, &newlen))
    return false;
  return newlen <= b->cap || buf_grow(b, newlen - b->cap);
}


bool buf_push(buf_t* b, u8 byte) {
  if (UNLIKELY(b->len >= b->cap) && UNLIKELY(!buf_grow(b, 1)))
    return false;
  b->bytes[b->len++] = byte;
  return true;
}


bool buf_nullterm(buf_t* b) {
  if (UNLIKELY(b->len >= b->cap) && UNLIKELY(!buf_grow(b, 1)))
    return false;
  b->bytes[b->len] = 0;
  return true;
}


u8* nullable buf_alloc(buf_t* b, usize len) {
  usize newlen;
  if (check_add_overflow(b->len, len, &newlen))
    return NULL;
  if (len > b->cap && UNLIKELY(!buf_grow(b, len - b->cap)))
    return NULL;
  u8* p = b->bytes + b->len;
  b->len = newlen;
  return p;
}


bool buf_append(buf_t* b, const void* src, usize len) {
  void* p = buf_alloc(b, len);
  if UNLIKELY(p == NULL)
    return false;
  memcpy(p, src, len);
  return true;
}


bool buf_print(buf_t* b, const char* cstr) {
  return buf_append(b, cstr, strlen(cstr));
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
