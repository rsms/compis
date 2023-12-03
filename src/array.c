// SPDX-License-Identifier: Apache-2.0
#include "colib.h"
#include "array.h"


#if DEBUG
  #define assert_memalloc(a, ma) ( \
    ((a)->ma == NULL) ? (a)->ma = ma : \
    assertf((a)->ma == ma, "mixed memory allocators used with array %p", (a)) \
  )
#else
  #define assert_memalloc(a, ma) ((void)0)
#endif


void array_init(array_t* a) {
  a->ptr = NULL;
  a->len = 0;
  a->cap = 0;
}


void _array_dispose(array_t* a, memalloc_t ma, u32 elemsize) {
  assert_memalloc(a, ma);
  mem_t m = { .p = a->ptr, .size = a->cap * elemsize };
  mem_free(ma, &m);
  a->ptr = NULL;
  a->len = 0;
  a->cap = 0;
}


static bool _array_resize(array_t* a, memalloc_t ma, u32 elemsize, u32 newcap) {
  assert(newcap >= a->len);

  if (a->cap == newcap)
    return true;

  usize newsize;
  if (check_mul_overflow((usize)newcap, (usize)elemsize, &newsize))
    return false;

  mem_t m = { .p = a->ptr, .size = (usize)a->cap * (usize)elemsize };
  //dlog("%s oldsize %zu, newsize %zu", __FUNCTION__, m.size, newsize);
  if (!mem_resize(ma, &m, newsize))
    return false;

  a->ptr = m.p;
  a->cap = (u32)(m.size / (usize)elemsize);
  return true;
}


bool _array_grow(array_t* a, memalloc_t ma, u32 elemsize, u32 extracap) {
  assert_memalloc(a, ma);

  u32 newcap;
  if (a->cap == 0) {
    // initial allocation
    #ifdef CO_DEBUG_ARRAY_MINALLOC
      // useful for testing out-of-bounds access along with asan
      newcap = extracap;
    #else
      const u32 ideal_nbyte = 64;
      newcap = MAX(extracap, ideal_nbyte / elemsize);
    #endif
  } else {
    // grow allocation
    usize currsize = (usize)a->cap * (usize)elemsize;
    usize extrasize;
    if (check_mul_overflow((usize)extracap, (usize)elemsize, &extrasize))
      return false;
    if (currsize < 65536 && extrasize < 65536/2) {
      // double capacity until we hit 64KiB
      newcap = (a->cap >= extracap) ? a->cap * 2 : a->cap + extracap;
    } else {
      u32 addlcap = MAX(65536u / elemsize, CEIL_POW2(extracap));
      if (check_add_overflow(a->cap, addlcap, &newcap)) {
        // try adding exactly what is needed (extracap)
        if (check_add_overflow(a->cap, extracap, &newcap))
          return false;
      }
    }
  }

  //dlog("%s(extracap=%u) cap %u -> %u", __FUNCTION__, extracap, a->cap, newcap);
  assert(newcap - a->cap >= extracap);
  return _array_resize(a, ma, elemsize, newcap);
}


bool _array_shrinkwrap(array_t* a, memalloc_t ma, usize elemsize) {
  return _array_resize(a, ma, elemsize, a->len);
}


bool _array_reserve(array_t* a, memalloc_t ma, u32 elemsize, u32 minavail) {
  usize avail = a->cap - a->len;
  if (avail >= minavail)
    return true;
  usize extracap = minavail - avail;
  return _array_grow(a, ma, elemsize, extracap);
}


bool _array_reserve_exact(array_t* a, memalloc_t ma, u32 elemsize, u32 minavail) {
  usize avail = a->cap - a->len;
  if (avail >= minavail)
    return true;
  usize newcap = a->cap + (minavail - avail);
  return _array_resize(a, ma, elemsize, newcap);
}


void* nullable _array_alloc(array_t* a, memalloc_t ma, u32 elemsize, u32 len) {
  if UNLIKELY(!_array_reserve(a, ma, elemsize, len))
    return NULL;
  void* p = a->ptr + a->len*elemsize;
  a->len += len;
  return p;
}

void* nullable _array_allocat(array_t* a, memalloc_t ma, u32 elemsize, u32 i, u32 len) {
  assert(i <= a->len);
  if UNLIKELY(i > a->len || !_array_reserve(a, ma, elemsize, len))
    return NULL;
  void* p = a->ptr + i*elemsize;
  if (i < a->len) {
    // examples:
    //   allocat [ 0 1 2 3 4 ] 5, 2 => [ 0 1 2 3 4 _ _ ]
    //   allocat [ 0 1 2 3 4 ] 1, 2 => [ 0 _ _ 1 2 3 4 ]
    //   allocat [ 0 1 2 3 4 ] 4, 2 => [ 0 1 2 3 _ _ 4 ]
    void* dst = a->ptr + (i + len)*elemsize;
    memmove(dst, p, (usize)((a->len - i) * elemsize));
  }
  a->len += len;
  return p;
}


void _array_remove(array_t* a, u32 elemsize, u32 start, u32 len) {
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


#define DEF_AROTATE(NAME, T)                                  \
  void NAME(T* const v, usize first, usize mid, usize last) { \
    assertf(first <= mid, "%zu <= %zu", first, mid);          \
    assertf(mid < last, "%zu < %zu", mid, last);              \
    usize next = mid;                                         \
    while (first != next) {                                   \
      /* [first, first+1] = [next+1, next] */                 \
      T tmp = v[first];                                       \
      v[first++] = v[next];                                   \
      v[next++] = tmp;                                        \
      if (next == last) next = mid;                           \
      else if (first == mid) mid = next;                      \
    }                                                         \
  }

DEF_AROTATE(_arotate32, u32)
DEF_AROTATE(_arotate64, u64)


u32 ptrarray_rindexof(const ptrarray_t* a, const void* value) {
  for (u32 i = a->len; i;) {
    if (a->v[--i] == value)
      return i;
  }
  return U32_MAX;
}


u32 u32array_rindexof(const u32array_t* a, u32 value) {
  for (u32 i = a->len; i;) {
    if (a->v[--i] == value)
      return i;
  }
  return U32_MAX;
}


void ptrarray_move_to_end(ptrarray_t* a, u32 i) {
  assert(a->len > 0);
  assert(i < a->len);
  if (i < a->len - 1)
    ptrarray_move(a, a->len - 1, i, i+1);
}


#define ARRAY_ELEM_PTR(elemsize, a, i) ( (a)->ptr + ((usize)(elemsize) * (usize)(i)) )


void* _array_sortedset_assign(
  array_t* a, memalloc_t ma, u32 elemsize,
  const void* valptr, array_sorted_cmp_t cmpf, void* ctx)
{
  // binary search
  isize insert_at_index = 0;
  u32 mid, low = 0, high = a->len;
  while (low < high) {
    mid = (low + high) / 2;
    void* existing = ARRAY_ELEM_PTR(elemsize, a, mid);
    int cmp = cmpf(valptr, existing, ctx);
    if (cmp == 0)
      return existing;
    if (cmp < 0) {
      high = mid;
      insert_at_index = mid;
    } else {
      low = mid + 1;
      insert_at_index = mid+1;
    }
  }
  void* p = _array_allocat(a, ma, elemsize, insert_at_index, 1);
  if (p)
    memset(p, 0, elemsize);
  return p;
}


void* _array_sortedset_lookup(
  const array_t* a, u32 elemsize,
  const void* valptr, u32* indexp, array_sorted_cmp_t cmpf, void* ctx)
{
  // binary search
  u32 mid, low = 0, high = a->len;
  while (low < high) {
    mid = (low + high) / 2;
    void* existing = ARRAY_ELEM_PTR(elemsize, a, mid);
    int cmp = cmpf(valptr, existing, ctx);
    if (cmp == 0) {
      *indexp = mid;
      return existing;
    }
    if (cmp < 0) {
      high = mid;
    } else {
      low = mid + 1;
    }
  }
  return NULL;
}


// isize ptrarray_sortedset_indexof(const ptrarray_t* a, const void* value) {
//   // binary search
//   safecheck((usize)a->len <= ISIZE_MAX);
//   u32 mid, low = 0, high = a->len;
//   while (low < high) {
//     mid = (low + high) / 2;
//     void* existing = a->v[mid];
//     if (value == existing)
//       return (isize)mid;
//     if ((uintptr)value < (uintptr)existing) {
//       high = mid;
//     } else {
//       low = mid + 1;
//     }
//   }
//   return -1;
// }


static int str_cmp(const char** a, const char** b, void* ctx) {
  return strcmp(*a, *b);
}

bool ptrarray_sortedset_addcstr(ptrarray_t* a, memalloc_t ma, const char* str) {
  const char** vp = array_sortedset_assign(
    const char*, a, ma, &str, (array_sorted_cmp_t)str_cmp, NULL);
  if UNLIKELY(!vp)
    return false;
  if (*vp)
    return true;
  return (*vp = mem_strdup(ma, slice_cstr(str), 0)) != NULL;
}


static int ptr_cmp(const void** a, const void** b, void* ctx) {
  return *a == *b ? 0 : *a < *b ? -1 : 1;
}

bool ptrarray_sortedset_addptr(
  ptrarray_t* a, memalloc_t ma, const void* ptr, bool* nullable added_out)
{
  const void** vp = array_sortedset_assign(
    const void*, a, ma, &ptr, (array_sorted_cmp_t)ptr_cmp, NULL);
  if UNLIKELY(!vp)
    return false;
  if (added_out)
    *added_out = (*vp == NULL);
  *vp = ptr;
  return true;
}


static int u32_cmp(const u32* a, const u32* b, void* ctx) {
  return *a == *b ? 0 : *a < *b ? -1 : 1;
}

bool u32array_sortedset_add(u32array_t* a, memalloc_t ma, u32 v) {
  u32* vp = array_sortedset_assign(u32, a, ma, &v, (array_sorted_cmp_t)u32_cmp, NULL);
  if UNLIKELY(!vp)
    return false;
  *vp = v;
  return true;
}


#ifdef CO_ENABLE_TESTS
UNITTEST_DEF(array_sortedset) {
  uintptr insert_data[] = {
    0x600003098340,
    0x6000030982c0,
    0x600003098300,
    0x600003098280,
  };

  memalloc_t ma = memalloc_default();

  ptrarray_t a = {0};
  for (usize i = 0; i < countof(insert_data); i++) {
    bool added;
    uintptr v = insert_data[i];
    bool ok = ptrarray_sortedset_addptr(&a, ma, (const void*)v, &added);
    safecheck(ok);
    safecheck(added);
  }

  for (u32 i = 0; i < a.len; i++) {
    //log("a[%u] = %p", i, a.v[i]);
    if (i > 0)
      safecheck((uintptr)a.v[i] > (uintptr)a.v[i-1]);
  }

  // for (u32 i = 0; i < a.len; i++)
  //   safecheck(i == (usize)ptrarray_sortedset_indexof(&a, a.v[i]));

  ptrarray_dispose(&a, ma);
}
#endif // CO_ENABLE_TESTS
