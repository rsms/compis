// SPDX-License-Identifier: Apache-2.0
#include "colib.h"
#include "str.h"


// str_t should be castable to mem_t
static_assert(offsetof(str_t,p) == offsetof(mem_t,p), "");
static_assert(offsetof(str_t,cap) == offsetof(mem_t,size), "");


str_t str_makelen(const char* p, usize len) {
  safecheck(len < USIZE_MAX);
  mem_t m = mem_alloc(STR_MEMALLOC, (usize)len + 1/*nullterm*/);
  if LIKELY(m.p) {
    memcpy(m.p, p, len);
    ((char*)m.p)[len] = 0;
  }
  return (str_t){
    .p = m.p,
    .cap = m.size,
    .len = len * (usize)!!m.p,
  };
}


static bool _str_grow(str_t* s, usize extracap) {
  usize newcap;
  if (s->cap == 0) {
    newcap = MAX(sizeof(usize), MIN(CEIL_POW2(extracap), MAX(extracap, 256)));
  } else if (s->cap < extracap || check_mul_overflow(s->cap, 2lu, &newcap)) {
    // either the current capacity is less than what we need or cap>USIZE_MAX/2
    if (check_add_overflow(s->cap, CEIL_POW2(extracap), &newcap)) {
      // fall back to exact growth
      if (check_add_overflow(s->cap, extracap, &newcap))
        return false;
    }
  }
  return mem_resize(STR_MEMALLOC, (mem_t*)s, newcap);
}


bool str_push(str_t* s, char c) {
  if (UNLIKELY(s->len + 1/*nullterm*/ >= s->cap) && UNLIKELY(!_str_grow(s, 1)))
    return false;
  s->p[s->len] = c;
  s->p[++s->len] = 0;
  return true;
}


bool str_ensure_avail(str_t* s, usize minavail) {
  usize mincap;
  if (check_add_overflow(s->len + 1/*nullterm*/, minavail, &mincap))
    return false;
  return mincap <= s->cap || _str_grow(s, mincap - s->cap);
}


char* nullable str_reserve(str_t* s, usize len) {
  if (!str_ensure_avail(s, len))
    return NULL;
  char* p = s->p;
  s->len += len;
  s->p[s->len] = 0;
  return p;
}


bool str_appendlen(str_t* s, const char* src, usize len) {
  if (!str_ensure_avail(s, len))
    return false;
  memcpy(s->p + s->len, src, len);
  s->len += len;
  s->p[s->len] = 0;
  return true;
}


bool str_appendv(str_t* s, char glue, usize count, va_list ap) {
  bool ok = true;
  if (glue) {
    while (count--) {
      const char* part = assertnotnull(va_arg(ap, const char*));
      usize len = strlen(part) + 1; // include null char when copying
      usize gluew = (usize)!!count; // no glue char for last part
      ok &= str_appendlen(s, part, len - (usize)!count);
      s->p[s->len - gluew] = glue * (char)gluew;
    }
  } else {
    while (count--) {
      const char* part = assertnotnull(va_arg(ap, const char*));
      ok &= str_append(s, part);
    }
  }
  return ok;
}


bool str_appendstrings(str_t* s, char glue, usize count, ...) {
  va_list ap;
  va_start(ap, count);
  bool ok = str_appendv(s, glue, count, ap);
  va_end(ap);
  return ok;
}
