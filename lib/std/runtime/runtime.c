#define DEBUG_RUNTIME // XXX
#include "runtime.h"
#if !__STDC_HOSTED__
  #error Not yet implemented for freestanding
#else
  #include <stdio.h>
  #include <stdlib.h> // malloc et al
#endif

_Noreturn void __co_panic(__co_str msg) {
  fwrite("panic: ", strlen("panic: "), 1, stderr);
  fwrite(msg.ptr, msg.len, 1, stderr);
  putc('\n', stderr);
  fflush(stderr);
  __builtin_abort();
}

strong_alias(__co_panic, panic);

_Noreturn void __co_panic_out_of_bounds(void) {
  __co_panic(__CO_X_STR(u8"out of bounds access"));
}

_Noreturn void __co_panic_null(void) {
  __co_panic(__CO_X_STR(u8"null pointer"));
}


void* __co_mem_dup(const void* src, __co_uint size) {
  void* ptr = malloc(size);
  memcpy(ptr, src, size);
  dlog("%p (%lu B) -> %p (%lu B)", src, size, ptr, size);
  return ptr;
}

void __co_mem_free(void* ptr, __co_uint size) {
  dlog("%p (%lu B)", ptr, size);
  free(ptr);
}


void _print(__co_str msg) {
  fwrite(msg.ptr, msg.len, 1, stdout);
  fputc('\n', stdout);
}


bool __co_builtin_reserve(void* arrayptr, __co_uint elemsize, __co_uint cap) {
  // Defined for dynamic arrays.
  // Requests that arrayptr->cap >= cap.
  //
  // interpret as [u8], type is irrelevant; we use elemsize
  struct _coAh* a = arrayptr;
  if (a->cap < cap) {
    __co_uint nbyte;
    if (check_mul_overflow(cap, elemsize, &nbyte))
      return false;
    nbyte = ALIGN2(nbyte, sizeof(void*));
    void* p = realloc(a->ptr, nbyte);
    if (!p) {
      dlog("realloc %p (%lu B) -> FAILED (%lu B)", a->ptr, a->cap * elemsize, nbyte);
      return false;
    }
    dlog("realloc %p (%lu B) -> %p (%lu B)", a->ptr, a->cap * elemsize, p, nbyte);
    a->ptr = p;
    a->cap = nbyte / elemsize;
  }
  return true;
}


bool __co_builtin_resize(void* arrayptr, __co_uint elemsize, __co_uint len) {
  struct _coAh* a = arrayptr;
  if (len > a->len) {
    // grow
    if (a->cap < len && UNLIKELY(!__co_builtin_reserve(arrayptr, elemsize, len)))
      return false;
    // zero newly allocated memory
    memset((void*)a->ptr + a->len*elemsize, 0, (len - a->len)*elemsize);
  }
  a->len = len;
  return true;
}


// __add__(a Seq<T>, b Seq<T>) ?[T]
struct _coOAh __co_builtin_seq___add__(
  const void* aptr, __co_uint alen,
  const void* bptr, __co_uint blen,
  __co_uint elemsize
)
{
  __co_uint len;
  if (check_add_overflow(alen, blen, &len))
    goto err;

  __co_uint nbyte;
  if (check_mul_overflow(len, elemsize, &nbyte))
    goto err;
  nbyte = ALIGN2(nbyte, sizeof(void*));

  void* p = malloc(nbyte);
  if (!p) {
    dlog("malloc (%lu B) FAILED", nbyte);
    goto err;
  }
  dlog("malloc (%lu B) -> %p", nbyte, p);

  memcpy(p, aptr, alen * elemsize);
  memcpy(p + alen*elemsize, bptr, blen * elemsize);

  return (struct _coOAh){
    .ok = true,
    .v = {
      .cap = nbyte / elemsize,
      .len = len,
      .ptr = p,
    }
  };

err:
  return (struct _coOAh){};
}
