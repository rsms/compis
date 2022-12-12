// SPDX-License-Identifier: Apache-2.0
#include "c0lib.h"


#define BITSET_SIZE(cap)  ( sizeof(bitset_t) + (cap)/8 )
#define BITSET_CAP(size)  ( ( MIN(USIZE_MAX/8, (size) - sizeof(bitset_t)) ) * 8 )


bitset_t* nullable bitset_alloc(memalloc_t ma, usize cap) {
  // note: 1 bit reserved for "onheap" flag
  if ( cap > ((USIZE_MAX >> 1)/8) - IDIV_CEIL_X(sizeof(bitset_t),8) )
    return false;
  mem_t m = mem_alloc_zeroed(ma, BITSET_SIZE(cap));
  if (!m.p)
    return NULL;
  bitset_t* bs = m.p;
  bs->cap = BITSET_CAP(m.size);
  bs->onheap = 1;
  return bs;
}


bool bitset_grow(bitset_t** bs, memalloc_t ma, usize cap) {
  if ( cap > ((USIZE_MAX >> 1)/8) - IDIV_CEIL_X(sizeof(bitset_t),8) )
    return false;
  usize oldsize = BITSET_SIZE((*bs)->cap);
  usize newsize = BITSET_SIZE(cap);
  if (oldsize < newsize) {
    usize elemsize = 1;
    void* p = mem_resizev(ma, bs, oldsize, newsize, elemsize);
    if (!p)
      return false;
    *bs = p;
    (*bs)->cap = cap;
    (*bs)->onheap = 1;
  }
  return true;
}


bool bitset_copy(bitset_t** dstp, const bitset_t* src, memalloc_t ma) {
  if (!bitset_ensure_cap(dstp, ma, src->cap))
    return false;
  usize onheap = (*dstp)->onheap;
  memcpy_checked(*dstp, src, BITSET_SIZE(src->cap));
  (*dstp)->onheap = onheap;
  return true;
}
