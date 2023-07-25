// internal definitions for runtime
#pragma once
#include <coprelude.h>

#ifdef __APPLE__
  #define strong_alias(name, aliasname) \
    __asm__(".globl _" #aliasname); \
    __asm__(".set _" #aliasname ", _" #name); \
    extern __typeof(name) aliasname
#else
  #define strong_alias(name, aliasname) \
    extern __typeof__(name) aliasname __attribute__((__alias__(#name)))
#endif

#define weak_alias(name, aliasname) \
  extern __typeof(name) aliasname __attribute__((__weak__, __alias__(#name)))


typedef struct {__co_uint len; const u8* ptr;} __co_slice_u8_t; // &[u8]
typedef __co_slice_u8_t __co_str_t; // type str &[u8]


#define __CO_X_STR(cstr) ((__co_str_t){__builtin_strlen(cstr),(u8*)(cstr)})


// // u8"..." is of type char* prior to C23
// #if __STDC_VERSION__ < 202311L
//   typedef char char8_t;
// #endif
