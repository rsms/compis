// SPDX-License-Identifier: Apache-2.0
#include "c0lib.h"
#include "array.h"


void array_init(array_t* a) {
  a->ptr = NULL;
  a->cap = 0;
  a->len = 0;
}


void _array_dispose(array_t* a, memalloc_t ma, u32 elemsize) {
  mem_t m = { .p = a->ptr, .size = a->cap * elemsize };
  mem_free(ma, &m);
  a->ptr = NULL;
  a->cap = 0;
  a->len = 0;
}


bool _array_grow(array_t* a, memalloc_t ma, u32 elemsize, u32 extracap) {
  u32 newcap;
  if (a->cap == 0) {
    newcap = MAX(extracap, 32u);
  } else {
    if (check_mul_overflow(a->cap, (u32)2, &newcap))
      return false;
  }

  usize newsize;
  if (check_mul_overflow((usize)newcap, (usize)elemsize, &newsize))
    return false;

  mem_t m = { .p = a->ptr, .size = a->cap * elemsize };
  if (!mem_resize(ma, &m, newsize))
    return false;

  a->ptr = m.p;
  a->cap = m.size / elemsize;

  return true;
}


bool _array_reserve(array_t* a, memalloc_t ma, u32 elemsize, u32 minavail) {
  u32 newlen;
  if (check_add_overflow(a->len, minavail, &newlen))
    return false;
  return newlen <= a->cap || _array_grow(a, ma, elemsize, newlen - a->cap);
}


void* nullable _array_alloc(array_t* a, memalloc_t ma, u32 elemsize, u32 len) {
  if UNLIKELY(!_array_reserve(a, ma, elemsize, len))
    return NULL;
  void* p = a->ptr + a->len*elemsize;
  a->len += len;
  return p;
}


void _rarray_remove(array_t* a, u32 elemsize, u32 start, u32 len) {
  if (len == 0)
    return;
  safecheckf(start+len <= a->len, "end=%u > len=%u", start+len, a->len);
  if (start+len < a->len) {
    void* dst = a->ptr + elemsize*start;
    void* src = dst + elemsize*len;
    memmove(dst, src, elemsize*(a->len - start - len));
  }
  a->len -= len;
}


void _arotatemem(usize stride, void* v, usize first, usize mid, usize last) {
  assert(first <= mid); // if equal (zero length), do nothing
  assert(mid < last);
  usize tmp[16];
  usize next = mid;
  while (first != next) {
    // swap
    memcpy(tmp, v + first*stride, stride); // tmp = v[first]
    memcpy(v + first*stride, v + next*stride, stride); // v[first] = v[next]
    memcpy(v + next*stride, tmp, stride); // v[next] = tmp
    first++;
    next++;
    if (next == last) {
      next = mid;
    } else if (first == mid) {
      mid = next;
    }
  }
}


#define DEF_AROTATE(NAME, T)                                   \
  void NAME(T* const v, usize first, usize mid, usize last) {  \
    assert(first <= mid);                                      \
    assert(mid < last);                                        \
    usize next = mid;                                          \
    while (first != next) {                                    \
      T tmp = v[first]; v[first++] = v[next]; v[next++] = tmp; \
      if (next == last) next = mid;                            \
      else if (first == mid) mid = next;                       \
    }                                                          \
  }

DEF_AROTATE(_arotate32, u32)
DEF_AROTATE(_arotate64, u64)
