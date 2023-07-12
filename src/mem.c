// SPDX-License-Identifier: Apache-2.0
#include "colib.h"
#include <stdlib.h>

#if __APPLE__ || __BSD__
  #include <malloc/malloc.h>
  #define HAS_MALLOC_SIZE
#elif __linux__
  #include <malloc.h>
  #define HAS_MALLOC_USABLE_SIZE
#endif


#ifndef CO_MEM_MALLOC
  #define CO_MEM_MALLOC(size)            malloc(size)
  #define CO_MEM_CALLOC(count, elemsize) calloc(count, elemsize)
  #define CO_MEM_REALLOC(ptr, newsize)   realloc((ptr), (newsize))
  #define CO_MEM_FREE(ptr)               free(ptr)
#endif


// ——————————————————————————————————————————————————————————————————————————————————
// null allocator

static bool memalloc_null_impl(void* self, mem_t* m, usize size, bool _1) {
  if (m->p != NULL) {
    if (size == 0) {
      safefail("attempt to free memory %p to memalloc_null", m->p);
    } else {
      safefail("attempt to resize memory %p with memalloc_null", m->p);
    }
  }
  return false;
}

struct memalloc _memalloc_null = {
  .f = &memalloc_null_impl,
};


// ——————————————————————————————————————————————————————————————————————————————————
// bump allocator

typedef struct {
  struct memalloc ma;
  void*      end; // end of backing memory
  void*      ptr; // next allocation
  usize      cap;
  memalloc_t parent;
  int        flags;
} bump_allocator_t;

static_assert(sizeof(bump_allocator_t) == MEMALLOC_BUMP_OVERHEAD, "");


static bool bump_alloc(bump_allocator_t* a, mem_t* m, usize size, bool zeroed) {
  size = ALIGN2(size, sizeof(void*));
  if LIKELY(a->ptr + size <= a->end) {
    m->p = a->ptr;
    m->size = size;
    a->ptr += size;
    if (zeroed && !(a->flags & MEMALLOC_STORAGE_ZEROED))
      memset(m->p, 0, size);
    return true;
  }
  *m = (mem_t){0};
  return false;
}


static bool bump_resize(bump_allocator_t* a, mem_t* m, usize size, bool zeroed) {
  size = ALIGN2(size, sizeof(void*));
  if (size <= m->size) {
    m->size = size;
    return true;
  }

  // grow tail if we can
  if (a->ptr == m->p + m->size) {
    void* newptr = m->p + size;
    if UNLIKELY(newptr > a->end)
      return false; // no more free space left in slab
    a->ptr = newptr;
    if (zeroed && !(a->flags & MEMALLOC_STORAGE_ZEROED))
      memset(m->p + m->size, 0, size - m->size);
    m->size = size;
    return true;
  }

  // new allocation
  if UNLIKELY(a->ptr + size > a->end)
    return false; // no more free space left in slab
  memcpy(a->ptr, m->p, m->size);
  if (zeroed && !(a->flags & MEMALLOC_STORAGE_ZEROED))
    memset(a->ptr + m->size, 0, size - m->size);
  m->p = a->ptr;
  m->size = size;
  a->ptr += size;
  return true;
}


static bool bump_free(bump_allocator_t* a, mem_t* m, usize size, bool zeroed) {
  // free tail only
  if (a->ptr == m->p + m->size) {
    a->ptr -= m->size;
    if (a->flags & MEMALLOC_STORAGE_ZEROED)
      memset(a->ptr, 0, m->size);
  }
  *m = (mem_t){0};
  return true;
}


static bool _memalloc_bump_impl(void* self, mem_t* m, usize size, bool zeroed) {
  bump_allocator_t* a = (bump_allocator_t*)self;
  assertnotnull(m);
  if (m->p == NULL)
    return bump_alloc(a, m, size, zeroed);
  if (size != 0)
    return bump_resize(a, m, size, zeroed);
  return bump_free(a, m, size, zeroed);
}


static bump_allocator_t* make_memalloc_bump(void* storage, usize cap, int flags) {
  assert(cap >= sizeof(bump_allocator_t));
  bump_allocator_t* a = storage;
  a->ma.f = _memalloc_bump_impl;
  a->end = storage + cap;
  a->ptr = storage + sizeof(bump_allocator_t);
  a->cap = cap - sizeof(bump_allocator_t);
  a->flags = flags;
  return a;
}


memalloc_t memalloc_bump(void* storage, usize cap, int flags) {
  if (cap < sizeof(bump_allocator_t))
    return &_memalloc_null;
  bump_allocator_t* a = make_memalloc_bump(storage, cap, flags);
  a->parent = NULL;
  return (memalloc_t)a;
}


memalloc_t memalloc_bump_in(memalloc_t parent, usize cap, int flags) {
  assert((flags & MEMALLOC_STORAGE_ZEROED) == 0);
  mem_t m = mem_alloc(parent, cap);
  if (m.size < sizeof(bump_allocator_t))
    return &_memalloc_null;
  bump_allocator_t* a = make_memalloc_bump(m.p, m.size, flags);
  a->parent = parent;
  return (memalloc_t)a;
}


memalloc_t memalloc_bump_in_zeroed(memalloc_t parent, usize cap, int flags) {
  mem_t m = mem_alloc_zeroed(parent, cap);
  if (m.size < sizeof(bump_allocator_t))
    return &_memalloc_null;
  flags |= MEMALLOC_STORAGE_ZEROED;
  bump_allocator_t* a = make_memalloc_bump(m.p, m.size, flags);
  a->parent = parent;
  return (memalloc_t)a;
}


void memalloc_bump_in_dispose(memalloc_t ma) {
  bump_allocator_t* a = (bump_allocator_t*)ma;
  if (a->ma.f == &memalloc_null_impl)
    return;
  assertnotnull(a->parent);
  usize size = sizeof(bump_allocator_t) + a->cap;
  mem_freex(a->parent, MEM(a->end - size, size));
}


usize memalloc_bumpcap(memalloc_t ma) {
  bump_allocator_t* a = (bump_allocator_t*)ma;
  return a->cap;
}


usize memalloc_bumpuse(memalloc_t ma) {
  bump_allocator_t* a = (bump_allocator_t*)ma;
  return (usize)(uintptr)(a->ptr - (void*)a);
}


// ——————————————————————————————————————————————————————————————————————————————————
// libc allocator

static bool _memalloc_libc_impl(void* self, mem_t* m, usize size, bool zeroed) {
  assertnotnull(m);

  // allocate
  if (m->p == NULL) {
    if (size == 0)
      return true;
    // We use calloc instead of malloc + memset because many allocators
    // implement optimizations that avoids memset in cases where the
    // underlying memory is already zeroed. Saves time for large allocations.
    void* p = zeroed ? CO_MEM_CALLOC(1, size) : CO_MEM_MALLOC(size);
    if UNLIKELY(p == NULL)
      return false;
    // get actual size if supported
    #if defined(HAS_MALLOC_USABLE_SIZE)
      size = malloc_usable_size(p);
      assert(size > 0);
    #elif defined(HAS_MALLOC_SIZE)
      size = malloc_size(p);
      assert(size > 0);
    #endif
    m->p = p;
    m->size = size;
    return true;
  }

  // resize
  if (size != 0) {
    void* newp = CO_MEM_REALLOC(m->p, size);
    if UNLIKELY(!newp)
      return false;
    if (zeroed && size > m->size)
      memset(newp + m->size, 0, size - m->size);
    m->p = newp;
    m->size = size;
    return true;
  }

  // free
  safecheckf(m->p != NULL, "attempt to free NULL pointer of size %zu", m->size);
  void* p = m->p;
  size = m->size;
  #if CO_SAFE
    m->p = NULL;
    m->size = 0;
  #endif
  if (size > 0)
    CO_MEM_FREE(p);
  return true;
}

struct memalloc _memalloc_default = {
  .f = &_memalloc_libc_impl,
};

// ——————————————————————————————————————————————————————————————————————————————————
// ctx allocator

_Thread_local memalloc_t _memalloc_ctx = &_memalloc_default;

void _memalloc_scope_reset(memalloc_t* prevp) {
  // dlog("_memalloc_scope_reset prevp=%p (*prevp=%p)", prevp, *prevp);
  // note: prev->f==1 when memalloc_scope macro is used and scope ended
  assertnotnull(prevp);
  assertnotnull(*prevp);
  memalloc_ctx_set(*prevp);
}


// ——————————————————————————————————————————————————————————————————————————————————
// utility functions

char* nullable mem_strdup(memalloc_t ma, slice_t src, usize extracap) {
  char* dst = mem_alloc(ma, src.len + 1 + extracap).p;
  if UNLIKELY(dst == NULL)
    return NULL;
  memcpy(dst, src.p, src.len);
  dst[src.len] = 0;
  return dst;
}


char* nullable mem_strcat(memalloc_t ma, slice_t src1, slice_t src2) {
  usize size;
  if (check_add_overflow(src1.len, src2.len, &size) || size == USIZE_MAX)
    return NULL;
  char* dst = mem_alloc(ma, size + 1).p;
  if UNLIKELY(dst == NULL)
    return NULL;
  memcpy(dst, src1.p, src1.len);
  memcpy(dst + src1.len, src2.p, src2.len);
  dst[size] = 0;
  return dst;
}


void* nullable mem_allocv(memalloc_t ma, usize count, usize size) {
  if (check_mul_overflow(count, size, &size))
    return NULL;
  return mem_alloc_zeroed(ma, size).p;
}


void* nullable mem_resizev(
  memalloc_t ma, void* p, usize oldcount, usize newcount, usize size)
{
  assert_no_mul_overflow(oldcount, size);
  mem_t m = MEM(p, oldcount * size);
  if (check_mul_overflow(newcount, size, &size))
    return NULL;
  bool ok = ma->f(ma, &m, size, /*zeroed*/true);
  return (void*)((uintptr)m.p * (uintptr)ok);
}
