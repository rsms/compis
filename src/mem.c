#include "c0lib.h"
#include <stdlib.h>

#if __APPLE__ || __BSD__
  #include <malloc/malloc.h>
  #define HAS_MALLOC_SIZE
#elif __linux__
  #include <malloc.h>
  #define HAS_MALLOC_USABLE_SIZE
#endif


#ifndef C0_MEM_MALLOC
  #define C0_MEM_MALLOC(size)            malloc(size)
  #define C0_MEM_CALLOC(count, elemsize) calloc(count, elemsize)
  #define C0_MEM_REALLOC(ptr, newsize)   realloc((ptr), (newsize))
  #define C0_MEM_FREE(ptr)               free(ptr)
#endif


// ——————————————————————————————————————————————————————————————————————————————————
// null allocator

static bool memalloc_null_impl(memalloc_t _, mem_t* m, usize size, bool _1) {
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
// libc allocator


static bool _memalloc_libc_impl(memalloc_t _, mem_t* m, usize size, bool zeroed) {
  assertnotnull(m);

  // allocate
  if (m->p == NULL) {
    if (size == 0)
      return true;
    // We use calloc instead of malloc + memset because many allocators
    // implement optimizations that avoids memset in cases where the
    // underlying memory is already zeroed. Saves time for large allocations.
    void* p = zeroed ? C0_MEM_CALLOC(1, size) : C0_MEM_MALLOC(size);
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
    safecheckf(m->p != NULL, "attempt to resize NULL pointer of size %zu", m->size);
    void* newp = C0_MEM_REALLOC(m->p, size);
    if UNLIKELY(!newp)
      return false;
    if (zeroed && size > m->size)
      memset(m->p + m->size, 0, size - m->size);
    m->p = newp;
    m->size = size;
    return true;
  }

  // free
  safecheckf(m->p != NULL, "attempt to free NULL pointer of size %zu", m->size);
  if (m->size > 0)
    C0_MEM_FREE(m->p);
  #if C0_SAFE
    m->p = NULL;
    m->size = 0;
  #endif
  return true;
}

struct memalloc _memalloc_default = {
  .f = &_memalloc_libc_impl,
};

// ——————————————————————————————————————————————————————————————————————————————————
// ctx allocator

_Thread_local memalloc_t _memalloc_ctx = &_memalloc_default;

void _memalloc_scope_reset(memalloc_t* prevp) {
  //dlog("_memalloc_scope_reset prev->a=%p", prev->a);
  // note: prev->f==1 when memalloc_scope macro is used and scope ended
  assertnotnull(prevp);
  assertnotnull(*prevp);
  memalloc_t prev = *prevp;
  if (prev->f == (void*)(uintptr)1)
    memalloc_ctx_set(prev);
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
