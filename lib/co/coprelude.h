#pragma once
#if 0
#include <stdint.h>
typedef int8_t             i8;
typedef uint8_t            u8;
typedef int16_t            i16;
typedef uint16_t           u16;
typedef int32_t            i32;
typedef uint32_t           u32;
typedef signed long long   i64;
typedef unsigned long long u64;
typedef float              f32;
typedef double             f64;
#else
typedef char               i8;
typedef unsigned char      u8;
typedef short              i16;
typedef unsigned short     u16;
typedef int                i32;
typedef unsigned int       u32;
typedef signed long long   i64;
typedef unsigned long long u64;
typedef float              f32;
typedef double             f64;
typedef unsigned long      __co_uint;
typedef long               __co_int;
#endif

#ifndef NULL
  #define NULL ((void*)0)
#endif

#define true 1
#define false 0
#define bool _Bool

#define __co_noalias __restrict__
#define __co_unused  __attribute__((__unused__))

#define __co_pkg __attribute__((__visibility__("internal")))
#define __co_pub __attribute__((__visibility__("default")))

typedef struct {u64 len; const u8* ptr;} __co_x_u8_slice_t;
typedef __co_x_u8_slice_t __co_str_t; // type str &[u8]

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
