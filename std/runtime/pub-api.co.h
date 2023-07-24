// This is the runtime interface.
// It is included in all packages which use std/runtime.

_Noreturn void __co_panic(__co_str_t);
_Noreturn void __co_panic_out_of_bounds(void);
_Noreturn void __co_panic_null(void);

inline static void* __co_mem_dup(const void* src, __co_uint size) {
  void* ptr = __builtin_memcpy(__builtin_malloc(size), src, size);
  // __builtin_printf("__co_mem_dup(%p, %lu) => %p\n", src, size, ptr);
  return ptr;
}

inline static void __co_mem_free(void* ptr, __co_uint size) {
  // __builtin_printf("__co_mem_free(%p, %lu)\n", ptr, size);
  __builtin_free(ptr);
}

inline static void __co_checkbounds(u64 len, u64 index) {
  if (__builtin_expect(index >= len, false))
    __co_panic_out_of_bounds();
}

#define __co_checknull(x) ({ \
  __typeof__(x) x__ = (x); \
  (__builtin_expect(x__ == NULL, false) ? __co_panic_null() : ((void)0)), x__; \
})
