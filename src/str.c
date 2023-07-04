// SPDX-License-Identifier: Apache-2.0
#include "colib.h"
#include "str.h"


// str_t should be castable to mem_t
static_assert(offsetof(str_t,p) == offsetof(mem_t,p), "");
static_assert(offsetof(str_t,cap) == offsetof(mem_t,size), "");


str_t str_makelen(const char* p, usize len) {
  mem_t m = mem_alloc(STR_MEMALLOC, len + 1/*nullterm*/);
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


str_t str_makeempty(usize cap) {
  mem_t m = mem_alloc(STR_MEMALLOC, cap + 1/*nullterm*/);
  if LIKELY(m.p)
    ((char*)m.p)[0] = 0;
  return (str_t){
    .p = m.p,
    .cap = m.size,
    .len = 0,
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


bool str_ensure_avail(str_t* s, usize needavail) {
  if UNLIKELY(needavail + 1/*nullterm*/ > s->cap - s->len)
    return _str_grow(s, (needavail + 1) - (s->cap - s->len));
  return true;
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


bool str_prependlen(str_t* s, const char* src, usize len) {
  if (!str_ensure_avail(s, len))
    return false;
  memmove(s->p + len, s->p, s->len);
  memcpy(s->p, src, len);
  s->len += len;
  s->p[s->len] = 0;
  return true;
}


isize str_replace(str_t* s, slice_t olds, slice_t news, isize limit) {
  isize nsubs = 0;

  // first, calculate how much additional space we need
  usize additional_space_needed = 0;
  for (usize i = 0; i <= s->len - olds.len; i++) {
    if (memcmp(&s->p[i], olds.p, olds.len) == 0) {
      if (nsubs++ == limit)
        break;
      if (news.len > olds.len)
        additional_space_needed += news.len - olds.len;
      i += olds.len - 1;
    }
  }

  // if we need more space than we have, expand the input buffer
  if (!str_ensure_avail(s, additional_space_needed))
    return -1;

  // now we can replace the substrings
  isize nsub_countdown = nsubs + 1;
  for (usize i = 0; --nsub_countdown > 0 && i <= s->len - olds.len; i++) {
    if (memcmp(&s->p[i], olds.p, olds.len) == 0) {
      if (news.len != olds.len) {
        // move the rest of the string to make room for the new substring
        memmove(&s->p[i + news.len], &s->p[i + olds.len], s->len - i - olds.len + 1);
        s->len = s->len - olds.len + news.len;
      }
      memcpy(&s->p[i], news.p, news.len);
      i += news.len - 1;
    }
  }

  return nsubs;
}
