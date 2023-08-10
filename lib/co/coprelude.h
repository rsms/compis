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

struct __co_opt_b { bool ok; bool      v; };
struct __co_opt_a { bool ok; i8        v; };
struct __co_opt_h { bool ok; u8        v; };
struct __co_opt_s { bool ok; i16       v; };
struct __co_opt_t { bool ok; u16       v; };
struct __co_opt_l { bool ok; i32       v; };
struct __co_opt_m { bool ok; u32       v; };
struct __co_opt_x { bool ok; i64       v; };
struct __co_opt_y { bool ok; u64       v; };
struct __co_opt_i { bool ok; __co_int  v; };
struct __co_opt_j { bool ok; __co_uint v; };
struct __co_opt_f { bool ok; f32       v; };
struct __co_opt_d { bool ok; f64       v; };

struct __co_array_b { __co_uint cap, len; bool*      ptr; };
struct __co_array_a { __co_uint cap, len; i8*        ptr; };
struct __co_array_h { __co_uint cap, len; u8*        ptr; };
struct __co_array_s { __co_uint cap, len; i16*       ptr; };
struct __co_array_t { __co_uint cap, len; u16*       ptr; };
struct __co_array_l { __co_uint cap, len; i32*       ptr; };
struct __co_array_m { __co_uint cap, len; u32*       ptr; };
struct __co_array_x { __co_uint cap, len; i64*       ptr; };
struct __co_array_y { __co_uint cap, len; u64*       ptr; };
struct __co_array_i { __co_uint cap, len; __co_int*  ptr; };
struct __co_array_j { __co_uint cap, len; __co_uint* ptr; };
struct __co_array_f { __co_uint cap, len; f32*       ptr; };
struct __co_array_d { __co_uint cap, len; f64*       ptr; };

struct __co_slice_b { __co_uint len; const bool*      ptr; };
struct __co_slice_a { __co_uint len; const i8*        ptr; };
struct __co_slice_h { __co_uint len; const u8*        ptr; };
struct __co_slice_s { __co_uint len; const i16*       ptr; };
struct __co_slice_t { __co_uint len; const u16*       ptr; };
struct __co_slice_l { __co_uint len; const i32*       ptr; };
struct __co_slice_m { __co_uint len; const u32*       ptr; };
struct __co_slice_x { __co_uint len; const i64*       ptr; };
struct __co_slice_y { __co_uint len; const u64*       ptr; };
struct __co_slice_i { __co_uint len; const __co_int*  ptr; };
struct __co_slice_j { __co_uint len; const __co_uint* ptr; };
struct __co_slice_f { __co_uint len; const f32*       ptr; };
struct __co_slice_d { __co_uint len; const f64*       ptr; };

struct __co_mutslice_b { __co_uint len; bool*      ptr; };
struct __co_mutslice_a { __co_uint len; i8*        ptr; };
struct __co_mutslice_h { __co_uint len; u8*        ptr; };
struct __co_mutslice_s { __co_uint len; i16*       ptr; };
struct __co_mutslice_t { __co_uint len; u16*       ptr; };
struct __co_mutslice_l { __co_uint len; i32*       ptr; };
struct __co_mutslice_m { __co_uint len; u32*       ptr; };
struct __co_mutslice_x { __co_uint len; i64*       ptr; };
struct __co_mutslice_y { __co_uint len; u64*       ptr; };
struct __co_mutslice_i { __co_uint len; __co_int*  ptr; };
struct __co_mutslice_j { __co_uint len; __co_uint* ptr; };
struct __co_mutslice_f { __co_uint len; f32*       ptr; };
struct __co_mutslice_d { __co_uint len; f64*       ptr; };

typedef struct __co_slice_h __co_str_t;
