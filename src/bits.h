// SPDX-License-Identifier: Apache-2.0
#pragma once
ASSUME_NONNULL_BEGIN

typedef struct {
#if USIZE_MAX >= U64_MAX
  usize cap : 63; // number of bits at "bits"
#else
  usize cap : 31; // number of bits at "bits"
#endif
  usize onheap : 1;
  uintptr bits[];
} bitset_t;

bitset_t* nullable bitset_alloc(memalloc_t ma, usize cap);

// bitset_make allocates up to BITSET_STACK_SIZE bytes on stack,
// or uses heap memory from ma if cap > BITSET_STACK_CAP.
// bitset_t* nullable bitset_make(memalloc_t ma, usize cap)
#define bitset_make(ma, capacity) ({ \
  usize __cap = ALIGN2((usize)(capacity), (usize)8); \
  (__cap <= BITSET_STACK_CAP) ? ({ \
    bitset_t* bs = alloca(BITSET_STACK_SIZE); \
    memset(bs, 0, BITSET_STACK_SIZE); \
    bs->cap = __cap; \
    bs; \
  }) : \
  bitset_alloc(ma, __cap); \
})
#define BITSET_CAP_ALIGN   ( sizeof(uintptr)*8 )
#define BITSET_STACK_SIZE  ( (usize)64lu )
#define BITSET_STACK_CAP   ( (usize)( (BITSET_STACK_SIZE - sizeof(bitset_t)) * 8lu ) )

inline static void bitset_dispose(bitset_t* bs, memalloc_t ma) {
  if (bs->onheap)
    mem_freex(ma, MEM(bs, sizeof(bitset_t) + bs->cap*8));
}

bool bitset_grow(bitset_t** bs, memalloc_t ma, usize mincap);
inline static bool bitset_ensure_cap(bitset_t** bs, memalloc_t ma, usize mincap) {
  return LIKELY((*bs)->cap >= mincap) ? true : bitset_grow(bs, ma, mincap);
}
bool bitset_copy(bitset_t** dst, const bitset_t* src, memalloc_t ma);
inline static void bitset_clear(bitset_t* bs) { memset(bs->bits, 0, bs->cap/8); }

bool bitset_merge_union(bitset_t** dstp, const bitset_t* src, memalloc_t ma);
bool bitset_merge_xor(bitset_t** dstp, const bitset_t* src, memalloc_t ma);

// generic bytewise bit access functions
inline static bool bit_get(const void* bits, usize bit) {
  return !!( ((const u8*)bits)[bit / 8] & ((u8)1u << (bit % 8)) );
}
inline static void bit_set(void* bits, usize bit) {
  ((u8*)bits)[bit / 8] |= ((u8)1u << (bit % 8));
}
inline static void bit_clear(void* bits, usize bit) {
  ((u8*)bits)[bit / 8] &= ~((u8)1u << (bit % 8));
}

// bitset
inline static bool bitset_has(const bitset_t* bs, usize bit) {
  assert(bit < bs->cap);
  return bit_get(bs->bits, bit);
}
inline static void bitset_add(bitset_t* bs, usize bit) {
  assert(bit < bs->cap);
  bit_set(bs->bits, bit);
}
inline static void bitset_del(bitset_t* bs, usize bit) {
  assert(bit < bs->cap);
  bit_clear(bs->bits, bit);
}


ASSUME_NONNULL_END
