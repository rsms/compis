// This is the runtime interface.
// These symbols are available in all packages which use std/runtime.

__co_PKG _Noreturn void __co_panic(__co_str);
__co_PKG _Noreturn void __co_panic_out_of_bounds(void);
__co_PKG _Noreturn void __co_panic_null(void);

__co_PKG void* __co_mem_dup(const void* src, __co_uint size);
__co_PKG void __co_mem_free(void* ptr, __co_uint size);
__co_PKG bool __co_builtin_reserve(void* arrayptr, __co_uint elemsize, __co_uint cap);
__co_PKG bool __co_builtin_resize(void* arrayptr, __co_uint elemsize, __co_uint len);

__co_PKG struct _coOAh __co_builtin_seq___add__(
  const void* aptr, __co_uint alen,
  const void* bptr, __co_uint blen,
  __co_uint elemsize
);

inline static void __co_checkbounds(__co_uint len, __co_uint index) {
  if (__builtin_expect(len <= index, false))
    __co_panic_out_of_bounds();
}

// #define __co_checknull(x) ({ \
//   __typeof__(x) x__ = (x); \
//   (__builtin_expect(x__ == NULL, false) ? __co_panic_null() : ((void)0)), x__; \
// })
