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
#endif

#ifndef NULL
  #define NULL ((void*)0)
#endif

#define true 1
#define false 0
#define bool _Bool

#define _CO_NOALIAS __restrict__
#define _CO_UNUSED  __attribute__((__unused__))

#define _CO_VIS_PRI __attribute__((__unused__))
#define _CO_VIS_PKG __attribute__((__visibility__("internal")))
#define _CO_VIS_PUB __attribute__((__visibility__("default")))

__attribute__((__noreturn__)) void abort(void);

#define __nullcheck(x) ({ \
  __typeof__(x) x__ = (x); \
  (x__ == NULL ? abort() : ((void)0)), x__; \
})
