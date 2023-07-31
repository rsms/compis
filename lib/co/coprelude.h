#pragma once
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
