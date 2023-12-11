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

#define __co_NOALIAS __restrict__
#define __co_UNUSED  __attribute__((__unused__))

#define __co_PKG __attribute__((__visibility__("internal")))
#define __co_PUB __attribute__((__visibility__("default")))

// [T]
struct _coAb { __co_uint cap, len; bool*      ptr; };
struct _coAa { __co_uint cap, len; i8*        ptr; };
struct _coAh { __co_uint cap, len; u8*        ptr; };
struct _coAs { __co_uint cap, len; i16*       ptr; };
struct _coAt { __co_uint cap, len; u16*       ptr; };
struct _coAl { __co_uint cap, len; i32*       ptr; };
struct _coAm { __co_uint cap, len; u32*       ptr; };
struct _coAx { __co_uint cap, len; i64*       ptr; };
struct _coAy { __co_uint cap, len; u64*       ptr; };
struct _coAi { __co_uint cap, len; __co_int*  ptr; };
struct _coAj { __co_uint cap, len; __co_uint* ptr; };
struct _coAf { __co_uint cap, len; f32*       ptr; };
struct _coAd { __co_uint cap, len; f64*       ptr; };

// &[T]
struct _coSb { __co_uint len; const bool*      ptr; };
struct _coSa { __co_uint len; const i8*        ptr; };
struct _coSh { __co_uint len; const u8*        ptr; };
struct _coSs { __co_uint len; const i16*       ptr; };
struct _coSt { __co_uint len; const u16*       ptr; };
struct _coSl { __co_uint len; const i32*       ptr; };
struct _coSm { __co_uint len; const u32*       ptr; };
struct _coSx { __co_uint len; const i64*       ptr; };
struct _coSy { __co_uint len; const u64*       ptr; };
struct _coSi { __co_uint len; const __co_int*  ptr; };
struct _coSj { __co_uint len; const __co_uint* ptr; };
struct _coSf { __co_uint len; const f32*       ptr; };
struct _coSd { __co_uint len; const f64*       ptr; };

// mut&[T]
struct _coDb { __co_uint len; bool*      ptr; };
struct _coDa { __co_uint len; i8*        ptr; };
struct _coDh { __co_uint len; u8*        ptr; };
struct _coDs { __co_uint len; i16*       ptr; };
struct _coDt { __co_uint len; u16*       ptr; };
struct _coDl { __co_uint len; i32*       ptr; };
struct _coDm { __co_uint len; u32*       ptr; };
struct _coDx { __co_uint len; i64*       ptr; };
struct _coDy { __co_uint len; u64*       ptr; };
struct _coDi { __co_uint len; __co_int*  ptr; };
struct _coDj { __co_uint len; __co_uint* ptr; };
struct _coDf { __co_uint len; f32*       ptr; };
struct _coDd { __co_uint len; f64*       ptr; };

// ?T
struct _coOb { bool ok; bool      v; };
struct _coOa { bool ok; i8        v; };
struct _coOh { bool ok; u8        v; };
struct _coOs { bool ok; i16       v; };
struct _coOt { bool ok; u16       v; };
struct _coOl { bool ok; i32       v; };
struct _coOm { bool ok; u32       v; };
struct _coOx { bool ok; i64       v; };
struct _coOy { bool ok; u64       v; };
struct _coOi { bool ok; __co_int  v; };
struct _coOj { bool ok; __co_uint v; };
struct _coOf { bool ok; f32       v; };
struct _coOd { bool ok; f64       v; };

// // ?[T]
// struct _coOAb { bool ok; struct _coAb v; };
// struct _coOAa { bool ok; struct _coAa v; };
// struct _coOAh { bool ok; struct _coAh v; };
// struct _coOAs { bool ok; struct _coAs v; };
// struct _coOAt { bool ok; struct _coAt v; };
// struct _coOAl { bool ok; struct _coAl v; };
// struct _coOAm { bool ok; struct _coAm v; };
// struct _coOAx { bool ok; struct _coAx v; };
// struct _coOAy { bool ok; struct _coAy v; };
// struct _coOAi { bool ok; struct _coAi v; };
// struct _coOAj { bool ok; struct _coAj v; };
// struct _coOAf { bool ok; struct _coAf v; };
// struct _coOAd { bool ok; struct _coAd v; };

// // ?&[T]
// struct _coOSb { bool ok; struct _coSb v; };
// struct _coOSa { bool ok; struct _coSa v; };
// struct _coOSh { bool ok; struct _coSh v; };
// struct _coOSs { bool ok; struct _coSs v; };
// struct _coOSt { bool ok; struct _coSt v; };
// struct _coOSl { bool ok; struct _coSl v; };
// struct _coOSm { bool ok; struct _coSm v; };
// struct _coOSx { bool ok; struct _coSx v; };
// struct _coOSy { bool ok; struct _coSy v; };
// struct _coOSi { bool ok; struct _coSi v; };
// struct _coOSj { bool ok; struct _coSj v; };
// struct _coOSf { bool ok; struct _coSf v; };
// struct _coOSd { bool ok; struct _coSd v; };

// // ?mut&[T]
// struct _coODb { bool ok; struct _coDb v; };
// struct _coODa { bool ok; struct _coDa v; };
// struct _coODh { bool ok; struct _coDh v; };
// struct _coODs { bool ok; struct _coDs v; };
// struct _coODt { bool ok; struct _coDt v; };
// struct _coODl { bool ok; struct _coDl v; };
// struct _coODm { bool ok; struct _coDm v; };
// struct _coODx { bool ok; struct _coDx v; };
// struct _coODy { bool ok; struct _coDy v; };
// struct _coODi { bool ok; struct _coDi v; };
// struct _coODj { bool ok; struct _coDj v; };
// struct _coODf { bool ok; struct _coDf v; };
// struct _coODd { bool ok; struct _coDd v; };

// type str = &[u8]
typedef struct _coSh __co_str;
