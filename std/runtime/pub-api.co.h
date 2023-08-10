// This is the runtime interface.
// It is included in all packages which use std/runtime.

_Noreturn void __co_panic(__co_str_t);
_Noreturn void __co_panic_out_of_bounds(void);
_Noreturn void __co_panic_null(void);

void* __co_mem_dup(const void* src, __co_uint size);
void __co_mem_free(void* ptr, __co_uint size);

inline static void __co_checkbounds(__co_uint len, __co_uint index) {
  if (__builtin_expect(index >= len, false))
    __co_panic_out_of_bounds();
}

#define __co_checknull(x) ({ \
  __typeof__(x) x__ = (x); \
  (__builtin_expect(x__ == NULL, false) ? __co_panic_null() : ((void)0)), x__; \
})
