// internal definitions for runtime
#pragma once
#include <coprelude.h>

#define memset __builtin_memset
#define memcpy __builtin_memcpy
#define memmove __builtin_memmove
#define memcmp __builtin_memcmp
#define strlen __builtin_strlen
#define strcmp __builtin_strcmp
#define strncmp __builtin_strncmp


#define LIKELY(x)   (__builtin_expect((bool)(x), true))
#define UNLIKELY(x) (__builtin_expect((bool)(x), false))


static inline __attribute__((warn_unused_result))
bool __must_check_unlikely(bool unlikely) {
  return __builtin_expect(unlikely, false);
}

#define check_mul_overflow(a, b, dst) __must_check_unlikely(({  \
  __typeof__(a) a__ = (a);                 \
  __typeof__(b) b__ = (b);                 \
  __typeof__(dst) dst__ = (dst);           \
  (void) (&a__ == &b__);                   \
  (void) (&a__ == dst__);                  \
  __builtin_mul_overflow(a__, b__, dst__); \
}))

#define check_add_overflow(a, b, dst) __must_check_unlikely(({  \
  __typeof__(a) a__ = (a);                 \
  __typeof__(b) b__ = (b);                 \
  __typeof__(dst) dst__ = (dst);           \
  (void) (&a__ == &b__);                   \
  (void) (&a__ == dst__);                  \
  __builtin_add_overflow(a__, b__, dst__); \
}))


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

#define __CO_X_STR(cstr) ((__co_str){__builtin_strlen(cstr),(u8*)(cstr)})


// T ALIGN2<T>(T x, anyuint a) rounds up x to nearest a (a must be a power of two)
#define ALIGN2(x,a) ({ \
  __typeof__(x) atmp__ = (__typeof__(x))(a) - 1; \
  ( (x) + atmp__ ) & ~atmp__; \
})


#ifdef DEBUG_RUNTIME
  #define dlog(fmt, args...) \
    fprintf(stderr, "[runtime/%s] " fmt " (%s:%d)\n", \
      __FUNCTION__, ##args, __FILE__, __LINE__)
#else
  #define dlog(fmt, args...) ((void)0)
#endif


// // u8"..." is of type char* prior to C23
// #if __STDC_VERSION__ < 202311L
//   typedef char char8_t;
// #endif
