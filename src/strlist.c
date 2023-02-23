// SPDX-License-Identifier: Apache-2.0
#include "colib.h"
#include "strlist.h"


static void strlist_addnv(strlist_t* a, int count, va_list ap) {
  int len = 0;
  while (count--) {
    const char* arg = va_arg(ap, const char*);
    if (arg && *arg) {
      a->ok &= buf_append(&a->buf, arg, strlen(arg) + 1);
      len++;
    }
  }
  a->ok &= !check_add_overflow(a->len, len, &a->len);
}


strlist_t _strlist_make1(memalloc_t ma) {
  strlist_t strlist = { .buf = buf_make(ma), .ok = true };
  return strlist;
}

strlist_t _strlist_make2(memalloc_t ma, const char* a) {
  strlist_t al = _strlist_make1(ma);
  strlist_add(&al, a);
  return al;
}

strlist_t _strlist_makea(memalloc_t ma, int count, ...) {
  strlist_t a = _strlist_make1(ma);
  va_list ap;
  va_start(ap, count);
  strlist_addnv(&a, count, ap);
  va_end(ap);
  return a;
}


void strlist_dispose(strlist_t* a) {
  buf_dispose(&a->buf);
  #ifdef CO_SAFE
  // note: buf_dispose already zeroes itself in safe mode
  a->len = 0;
  a->ok = false;
  #endif
}


void strlist_add_raw(strlist_t* a, const char* src, usize len, int count) {
  if (len == 0)
    return;
  usize extralen = (usize)(src[len-1] != 0);
  u8* p = buf_alloc(&a->buf, len + extralen);
  dlog("buf_alloc(%zu) => %p", len + extralen, p);
  if UNLIKELY(!p) {
    a->ok = false;
  } else {
    dlog("cpy '%.*s'", (int)len, src);
    memcpy(p, src, len);
    if (extralen)
      p[len] = 0;
    assert(count > 0);
    a->ok &= !check_add_overflow(a->len, count, &a->len);
  }
}


void _strlist_add2(strlist_t* a, const char* cstr) {
  a->ok &= buf_append(&a->buf, cstr, strlen(cstr) + 1); // include null terminator
  a->ok &= !check_add_overflow(a->len, 1, &a->len);
}


void _strlist_adda(strlist_t* a, int count, ...) {
  va_list ap;
  va_start(ap, count);
  strlist_addnv(a, count, ap);
  va_end(ap);
}


ATTR_FORMAT(printf, 2, 3)
void strlist_addf(strlist_t* a, const char* fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  a->ok &= buf_vprintf(&a->buf, fmt, ap);
  va_end(ap);
  if UNLIKELY(!a->ok)
    return;
  assertf(a->buf.len < a->buf.cap, "buf_vprintf did not reserve space for null");
  a->buf.len++;
  a->ok &= !check_add_overflow(a->len, 1, &a->len);
}


void strlist_add_many(strlist_t* a, ...) {
  int len = 0;
  va_list ap;
  va_start(ap, a);
  for (const char* arg; (arg = va_arg(ap, const char*)) != NULL; ) {
    if (*arg) {
      a->ok &= buf_append(&a->buf, arg, strlen(arg) + 1);
      len++;
    }
  }
  va_end(ap);
  a->ok &= !check_add_overflow(a->len, len, &a->len);
}


void strlist_add_list(strlist_t* a, const strlist_t* b) {
  strlist_add_raw(a, b->buf.p, b->buf.len, b->len);
}


void strlist_add_array(strlist_t* a, const char*const* src, usize len) {
  if (len > I32_MAX || check_add_overflow(a->len, (int)len, &a->len)) {
    a->ok = false;
    return;
  }
  for (usize i = 0; i < len; i++)
    a->ok &= buf_append(&a->buf, src[i], strlen(src[i]) + 1);
}


void strlist_restore(strlist_t* a, const strlist_t snapshot) {
  a->buf.len = snapshot.buf.len;
  a->len = snapshot.len;
}


char* const* strlist_array(strlist_t* a) {
  // the index array is stored after the argument strings in a->buf's free space
  char** v = (char**)ALIGN2((uintptr)(a->buf.p + a->buf.len), sizeof(void*));
  if (a->_ap == (void*)v)
    return a->_ap;

  usize len = (usize)a->len + 1 + 1; // +1 for NULL terminator, +1 for alignment padding
  a->ok &= buf_reserve(&a->buf, len*sizeof(void*));

  if UNLIKELY(!a->ok) {
    a->len = 0;
    static const char last_resort = 0;
    return (char**)&last_resort;
  }

  v = (char**)ALIGN2((uintptr)(a->buf.p + a->buf.len), sizeof(void*));
  char** vp = v;

  char* sp = a->buf.chars;
  char* send = a->buf.chars + a->buf.len;
  char* sstart = sp;
  while (sp < send) {
    if (*sp++ == 0) {
      *vp++ = sstart;
      sstart = sp;
    }
  }
  *vp = NULL;
  assertf(sstart == send, "last entry is not null-terminated");
  return v;
}
