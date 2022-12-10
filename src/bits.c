// SPDX-License-Identifier: Apache-2.0
#include "c0lib.h"

bitset_t* nullable bitset_alloc(memalloc_t ma, u32 size) {
  mem_t m = mem_alloc_zeroed(ma, sizeof(bitset_t) + size);
  if (!m.p)
    return NULL;
  bitset_t* bs = m.p;
  bs->size = (u32)MIN((usize)U32_MAX, m.size - sizeof(bitset_t));
  return bs;
}
