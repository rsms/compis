#pragma once
#include <stdint.h>
#include <stdio.h>

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

#ifndef NULL
  #define NULL ((void*)0)
#endif

#define true 1
#define false 0
#define bool _Bool

__attribute__((noreturn)) void abort(void);
#define __nullcheck(x) ({ \
  __typeof__(x) x__ = (x); \
  (x__ == NULL ? abort() : ((void)0)), x__; \
})

// ——————————————————— internal ———————————————————————

inline static void _c0·drop(void* p) {
  printf("drop %p\n", p);
}
inline static void _c0·drop_opt(void* p) {
  if (p) _c0·drop(p);
}
