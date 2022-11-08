#pragma once
//—————————————————————————————————————————————————————————————————————————————————————
// types

#include <stdint.h>
#include <stdbool.h>
#include <sys/types.h>

typedef int8_t             i8;
typedef uint8_t            u8;
typedef int16_t            i16;
typedef uint16_t           u16;
typedef int32_t            i32;
typedef uint32_t           u32;
typedef signed long long   i64;
typedef unsigned long long u64;
typedef size_t             usize;
typedef ssize_t            isize;
typedef intptr_t           intptr;
typedef uintptr_t          uintptr;
typedef float              f32;
typedef double             f64;

//—————————————————————————————————————————————————————————————————————————————————————
// limits
#include <limits.h>

#define I8_MAX    0x7f
#define I16_MAX   0x7fff
#define I32_MAX   0x7fffffff
#define I64_MAX   0x7fffffffffffffff
#define ISIZE_MAX __LONG_MAX__

#define I8_MIN    (-0x80)
#define I16_MIN   (-0x8000)
#define I32_MIN   (-0x80000000)
#define I64_MIN   (-0x8000000000000000)
#define ISIZE_MIN (-__LONG_MAX__ -1L)

#define U8_MAX    0xff
#define U16_MAX   0xffff
#define U32_MAX   0xffffffff
#define U64_MAX   0xffffffffffffffff
#ifdef __SIZE_MAX__
  #define USIZE_MAX __SIZE_MAX__
#else
  #define USIZE_MAX (__LONG_MAX__ *2UL+1UL)
#endif

//—————————————————————————————————————————————————————————————————————————————————————
// va_args et al
#include <stdarg.h>

//—————————————————————————————————————————————————————————————————————————————————————
// compiler directives & attributes

#if __has_attribute(warn_unused_result)
  #define WARN_UNUSED __attribute__((warn_unused_result))
#else
  #define WARN_UNUSED
#endif

#if __has_attribute(unused)
  #define UNUSED __attribute__((unused))
#else
  #define UNUSED
#endif

#if __has_attribute(musttail) && !defined(__wasm__)
  // Note on "!defined(__wasm__)": clang 13 claims to have this attribute for wasm
  // targets but it's actually not implemented and causes an error.
  #define MUSTTAIL __attribute__((musttail))
#else
  #define MUSTTAIL
#endif

#if __has_attribute(fallthrough)
  #define FALLTHROUGH __attribute__((fallthrough))
#else
  #define FALLTHROUGH
#endif

#if !defined(__cplusplus)
  #define static_assert _Static_assert
#endif

// ATTR_FORMAT(archetype, string-index, first-to-check)
// archetype determines how the format string is interpreted, and should be printf, scanf,
// strftime or strfmon.
// string-index specifies which argument is the format string argument (starting from 1),
// while first-to-check is the number of the first argument to check against the format string.
// For functions where the arguments are not available to be checked (such as vprintf),
// specify the third parameter as zero.
#if __has_attribute(format)
  #define ATTR_FORMAT(archetype, fmtarg, checkarg) \
    __attribute__((format(archetype, fmtarg, checkarg)))
#else
  #define ATTR_FORMAT(archetype, fmtarg, checkarg)
#endif

#if __has_attribute(warn_unused_result)
  #define WARN_UNUSED_RESULT __attribute__((warn_unused_result))
#else
  #define WARN_UNUSED_RESULT
#endif

// UNLIKELY(integralexpr)->bool
#if __has_builtin(__builtin_expect)
  #define LIKELY(x)   (__builtin_expect((bool)(x), true))
  #define UNLIKELY(x) (__builtin_expect((bool)(x), false))
#else
  #define LIKELY(x)   (x)
  #define UNLIKELY(x) (x)
#endif

#ifdef __cplusplus
  #define EXTERN_C extern "C"
#else
  #define EXTERN_C
#endif

//—————————————————————————————————————————————————————————————————————————————————————
// nullability

#if defined(__clang__) && __has_feature(nullability)
  #ifndef nullable
    #define nullable _Nullable
  #endif
  #define ASSUME_NONNULL_BEGIN                                                \
    _Pragma("clang diagnostic push")                                              \
    _Pragma("clang diagnostic ignored \"-Wnullability-completeness\"")            \
    _Pragma("clang diagnostic ignored \"-Wnullability-inferred-on-nested-type\"") \
    _Pragma("clang assume_nonnull begin")
  #define ASSUME_NONNULL_END    \
    _Pragma("clang diagnostic pop") \
    _Pragma("clang assume_nonnull end")
#else
  #ifndef nullable
    #define nullable
  #endif
  #define C0_ASSUME_NONNULL_BEGIN
  #define C0_ASSUME_NONNULL_END
#endif

//—————————————————————————————————————————————————————————————————————————————————————
// fundamental macros

#ifndef countof
  #define countof(x) ((sizeof(x)/sizeof(0[x])) / ((usize)(!(sizeof(x) % sizeof(0[x])))))
#endif

#if __has_builtin(__builtin_offsetof)
  #undef offsetof
  #define offsetof __builtin_offsetof
#elif !defined(offsetof)
  #define offsetof(st, m) ((usize)&(((st*)0)->m))
#endif

// container_of returns a pointer to the parent struct of one of its members (ptr).
#define container_of(ptr, struct_type, struct_member) ({ \
  const __typeof__( ((struct_type*)0)->struct_member )* ptrx__ = (ptr); \
  (struct_type*)( (u8*)ptrx__ - offsetof(struct_type,struct_member) ); \
})

#define MAX(a,b) ( \
  __builtin_constant_p(a) && __builtin_constant_p(b) ? ((a) > (b) ? (a) : (b)) : \
  ({__typeof__ (a) _a = (a), _b = (b); _a > _b ? _a : _b; }) \
)
#define MIN(a,b) ( \
  __builtin_constant_p(a) && __builtin_constant_p(b) ? ((a) < (b) ? (a) : (b)) : \
  ({__typeof__ (a) _a = (a), _b = (b); _a < _b ? _a : _b; }) \
)

#define MAX_X(a,b)  ( (a) > (b) ? (a) : (b) )
#define MIN_X(a,b)  ( (a) < (b) ? (a) : (b) )

#define CONCAT_(x,y)  x##y
#define CONCAT(x,y)   CONCAT_(x,y)

#define c0_same_type(a, b) __builtin_types_compatible_p(__typeof__(a), __typeof__(b))

// __VARG_DISP allows writing functions with compile-time variable-count arguments
#define __VARG_DISP(a,...)   __VARG_CONCAT(a,__VARG_NARGS(__VA_ARGS__))(__VA_ARGS__)
#define __VARG_NARGS_X(a,b,c,d,e,f,g,h,n,...) n
#define __VARG_NARGS(...)    __VARG_NARGS_X(__VA_ARGS__,8,7,6,5,4,3,2,1,)
#define __VARG_CONCAT_X(a,b) a##b
#define __VARG_CONCAT(a,b)   __VARG_CONCAT_X(a,b)

// int c0_clz(ANYUINT x) counts leading zeroes in x,
// starting at the most significant bit position.
// If x is 0, the result is undefined.
#define c0_clz(x) ( \
  _Generic((x), \
    i8:   __builtin_clz,   u8:    __builtin_clz, \
    i16:  __builtin_clz,   u16:   __builtin_clz, \
    i32:  __builtin_clz,   u32:   __builtin_clz, \
    long: __builtin_clzl,  unsigned long: __builtin_clzl, \
    long long:  __builtin_clzll, unsigned long long:   __builtin_clzll \
  )(x) - ( 32 - MIN_X(4, (int)sizeof(__typeof__(x)))*8 ) \
)

// int c0_fls(ANYINT n) finds the Find Last Set bit (last = most-significant)
// (Note that this is not the same as c0_ffs(x)-1).
// e.g. c0_fls(0b1111111111111111) = 15
// e.g. c0_fls(0b1000000000000000) = 15
// e.g. c0_fls(0b1000000000000000) = 15
// e.g. c0_fls(0b1000) = 3
#define c0_fls(x)  ( (x) ? (int)(sizeof(__typeof__(x)) * 8) - c0_clz(x) : 0 )

// int ILOG2(ANYINT n) calculates the log of base 2, rounding down.
// e.g. ILOG2(15) = 3, ILOG2(16) = 4.
// Result is undefined if n is 0.
#define ILOG2(n) (c0_fls(n) - 1)

// ANYINT FLOOR_POW2(ANYINT x) rounds down x to nearest power of two.
// Returns 1 if x is 0.
#define FLOOR_POW2(x) ({ \
  __typeof__(x) xtmp__ = (x); \
  FLOOR_POW2_X(xtmp__); \
})
// FLOOR_POW2_X is a constant-expression implementation of FLOOR_POW2.
// When used as a constant expression, compilation fails if x is 0.
#define FLOOR_POW2_X(x) ( \
  ((x) <= 1) ? (__typeof__(x))1 : \
  ((__typeof__(x))1 << ILOG2(x)) \
)

// ANYINT CEIL_POW2(ANYINT x) rounds up x to nearest power of two.
// Returns 1 when x is 0.
// Returns 0 when x is larger than the max pow2 for x's type (e.g. >0x80000000 for u32)
#define CEIL_POW2(x) ({ \
  __typeof__(x) xtmp__ = (x); \
  CEIL_POW2_X(xtmp__); \
})
// CEIL_POW2_X is a constant-expression implementation of CEIL_POW2
#define CEIL_POW2_X(x) ( \
  ((x) <= (__typeof__(x))1) ? (__typeof__(x))1 : \
  ( ( ((__typeof__(x))1 << \
          ILOG2( ((x) - ((x) == (__typeof__(x))1) ) - (__typeof__(x))1) \
      ) - (__typeof__(x))1 ) << 1 ) \
  + (__typeof__(x))2 \
)

// bool IS_POW2(T x) returns true if x is a power-of-two value
#define IS_POW2(x)    ({ __typeof__(x) xtmp__ = (x); IS_POW2_X(xtmp__); })
#define IS_POW2_X(x)  ( ((x) & ((x) - 1)) == 0 )

// T ALIGN2<T>(T x, anyuint a) rounds up x to nearest a (a must be a power of two)
#define ALIGN2(x,a) ({ \
  __typeof__(x) atmp__ = (__typeof__(x))(a) - 1; \
  ( (x) + atmp__ ) & ~atmp__; \
})
#define ALIGN2_X(x,a) ( \
  ( (x) + ((__typeof__(x))(a) - 1) ) & ~((__typeof__(x))(a) - 1) \
)

//—————————————————————————————————————————————————————————————————————————————————————
// debugging
#include <stdio.h>

// panic prints msg to stderr and calls TRAP()
#define panic(fmt, args...) _panic(__FILE__, __LINE__, __FUNCTION__, fmt, ##args)

// void assert(expr condition)
#undef assert
#define comptime_assert(condition, msg) _Static_assert(condition, msg)
#if defined(DEBUG)
  #ifdef NDEBUG
    #warning both DEBUG and NDEBUG defined
  #endif
  #undef DEBUG
  #undef NDEBUG
  #undef C0_SAFE
  #define DEBUG 1
  #define C0_SAFE 1

  #define _assertfail(fmt, args...) \
    _panic(__FILE__, __LINE__, __FUNCTION__, "Assertion failed: " fmt, args)
  // Note: we can't use ", ##args" above in either clang nor gcc for some reason,
  // or else certain applications of this macro are not expanded.

  #define assertf(cond, fmt, args...) \
    (UNLIKELY(!(cond)) ? _assertfail(fmt " (%s)", ##args, #cond) : ((void)0))

  #define assert(cond) \
    (UNLIKELY(!(cond)) ? _assertfail("%s", #cond) : ((void)0))

  #define assertcstreq(cstr1, cstr2) ({                  \
    const char* cstr1__ = (cstr1);                       \
    const char* cstr2__ = (cstr2);                       \
    if (UNLIKELY(strcmp(cstr1__, cstr2__) != 0))         \
      _assertfail("\"%s\" != \"%s\"", cstr1__, cstr2__); \
  })

  #define assertnull(a)  assert((a) == NULL)

  #ifdef __cplusplus
    #define assertnotnull(a) ({                                         \
      __typeof__(a) nullable val__ = (a);                               \
      UNUSED const void* valp__ = val__; /* build bug on non-pointer */ \
      if (UNLIKELY(val__ == NULL))                                      \
        _assertfail("%s != NULL", #a);                                  \
      val__; })
  #else
    #define assertnotnull(a) ({                                          \
      __typeof__(*(a))* nullable val__ = (a);                            \
      UNUSED const void* valp__ = val__; /* build bug on non-pointer */  \
      if (UNLIKELY(val__ == NULL))                                       \
        _assertfail("%s != NULL", #a);                                   \
      val__; })
  #endif

  // assert_no_add_overflow(T a, Y b)
  #if __has_builtin(__builtin_add_overflow_p)

    #define assert_no_add_overflow(a, b) \
      assertf(!__builtin_add_overflow_p((a), (b), (__typeof__((a)+(b)))0), \
        "0x%llx + 0x%llx overflows", (u64)(a), (u64)(b))
    #define assert_no_sub_overflow(a, b) \
      assertf(!__builtin_sub_overflow_p((a), (b), (__typeof__((a)+(b)))0), \
        "0x%llx - 0x%llx overflows", (u64)(a), (u64)(b))
    #define assert_no_mul_overflow(a, b) \
      assertf(!__builtin_mul_overflow_p((a), (b), (__typeof__((a)+(b)))0), \
        "0x%llx * 0x%llx overflows", (u64)(a), (u64)(b))

  #elif __has_builtin(__builtin_add_overflow)

    #define assert_no_add_overflow(a, b) ({ \
      __typeof__((a)+(b)) tmp__; \
      assertf(!__builtin_add_overflow((a), (b), &tmp__), \
        "0x%llx + 0x%llx overflows", (u64)(a), (u64)(b)); \
    })
    #define assert_no_sub_overflow(a, b) ({ \
      __typeof__((a)+(b)) tmp__; \
      assertf(!__builtin_sub_overflow((a), (b), &tmp__), \
        "0x%llx - 0x%llx overflows", (u64)(a), (u64)(b)); \
    })
    #define assert_no_mul_overflow(a, b) ({ \
      __typeof__((a)+(b)) tmp__; \
      assertf(!__builtin_mul_overflow((a), (b), &tmp__), \
        "0x%llx * 0x%llx overflows", (u64)(a), (u64)(b)); \
    })

  #else
    // best effort; triggers ubsan
    #define assert_no_add_overflow(a, b) ({ \
      __typeof__((a)+(b)) tmp__ = (i64)(a) + (i64)(b); \
      assertf((u64)tmp__ >= (u64)(a), "0x%llx + 0x%llx overflows", (u64)(a), (u64)(b));\
    })
    #define assert_no_sub_overflow(a, b) ({ \
      __typeof__((a)+(b)) tmp__ = (i64)(a) - (i64)(b); \
      assertf((u64)tmp__ <= (u64)(a), "0x%llx - 0x%llx overflows", (u64)(a), (u64)(b));\
    })
    #define assert_no_mul_overflow(a, b) ({ \
      UNUSED __typeof__((a)+(b)) tmp__ = (i64)(a) * (i64)(b); \
    })
  #endif

#else /* !defined(NDEBUG) */
  #undef DEBUG
  #undef NDEBUG
  #define NDEBUG 1
  #define assert(cond)                 ((void)0)
  #define assertf(cond, fmt, ...)      ((void)0)
  #define assertcstreq(a,b)            ((void)0)
  #define assertnull(a)                ((void)0)
  #define assertnotnull(a)             ({ a; }) /* note: (a) causes "unused" warnings */
  #define assert_no_add_overflow(a, b) ((void)0)
  #define assert_no_sub_overflow(a, b) ((void)0)
  #define assert_no_mul_overflow(a, b) ((void)0)
#endif /* !defined(NDEBUG) */

// C0_SAFE -- checks enabled in "debug" and "safe" builds (but not in "fast" builds.)
//
// void safecheck(COND)                         stripped from non-safe builds
// void safecheckf(COND, const char* fmt, ...)  stripped from non-safe builds
// void safecheckx(COND)                        included in non-safe builds w/o check
// void safecheckxf(COND, const char* fmt, ...) included in non-safe builds w/o check
// typeof(EXPR) safecheckexpr(EXPR, EXPECT)     included in non-safe builds w/o check
// typeof(EXPR) safechecknotnull(EXPR)          included in non-safe builds w/o check
//
#if defined(C0_SAFE)
  #undef C0_SAFE
  #define C0_SAFE 1
  #define safefail(fmt, args...) _panic(__FILE__, __LINE__, __FUNCTION__, fmt, ##args)
  #define safecheckf(cond, fmt, args...)  ( (cond) ? ((void)0) : safefail(fmt, ##args) )
  #define safecheckxf safecheckf
  #define safecheckx safecheck
  #ifdef DEBUG
    #define safecheck(cond)  ( (cond) ? ((void)0) : safefail("safecheck (%s)", #cond) )
    #define safecheckexpr(expr, expect) ({                                        \
      __typeof__(expr) val__ = (expr);                                            \
      safecheckf(val__ == expect, "unexpected value (%s != %s)", #expr, #expect); \
      val__; })
    #define safechecknotnull(a) ({                                           \
      __typeof__(a) val__ = (a);                                             \
      UNUSED const void* valp__ = val__; /* build bug on non-pointer */ \
      safecheckf(val__ != NULL, "unexpected NULL (%s)", #a);                 \
      val__; })
  #else
    #define safecheck(cond) ( (cond) ? ((void)0) : safefail("safecheck") )
    #define safecheckexpr(expr, expect) ({ \
      __typeof__(expr) val__ = (expr); safecheck(val__ == expect); val__; })
    #define safechecknotnull(a) ({                                           \
      __typeof__(a) val__ = (a);                                             \
      UNUSED const void* valp__ = val__; /* build bug on non-pointer */ \
      safecheckf(val__ != NULL, "NULL");                                     \
      val__; })
  #endif
#else
  #define safefail(fmt, args...)      ((void)0)
  #define safecheckf(...)             ((void)0)
  #define safecheck(cond)             ((void)0)
  #define safecheckx(cond)            do{ UNUSED bool _ = (cond); }while(0)
  #define safecheckxf(cond, ...)      do{ UNUSED bool _ = (cond); }while(0)
  #define safecheckexpr(expr, expect) (expr) /* intentionally complain if not used */
  #define safechecknotnull(a)         ({ a; }) /* note: (a) causes "unused" warnings */
#endif

// void dlog(const char* fmt, ...)
#ifdef DEBUG
  #define dlog(fmt, args...) _dlog(__FILE__, __LINE__, fmt, ##args)
#else
  #define dlog(fmt, ...) ((void)0)
#endif

// void log(const char* fmt, ...)
#define log(fmt, args...) fprintf(stderr, fmt "\n", ##args)

// no more includes beyond this point; enable default non-nullable pointers
ASSUME_NONNULL_BEGIN

EXTERN_C _Noreturn void _panic(
  const char* file, int line, const char* fun, const char* fmt, ...)
  ATTR_FORMAT(printf, 4, 5);

EXTERN_C void _dlog(
  const char* file, int line, const char* fmt, ...) ATTR_FORMAT(printf, 3, 4);

//—————————————————————————————————————————————————————————————————————————————————————
// overflow checking

static inline WARN_UNUSED_RESULT bool __must_check_unlikely(bool unlikely) {
  return UNLIKELY(unlikely);
}

#define check_add_overflow(a, b, dst) __must_check_unlikely(({  \
  __typeof__(a) a__ = (a);                 \
  __typeof__(b) b__ = (b);                 \
  __typeof__(dst) dst__ = (dst);           \
  (void) (&a__ == &b__);                   \
  (void) (&a__ == dst__);                  \
  __builtin_add_overflow(a__, b__, dst__); \
}))

#define check_sub_overflow(a, b, dst) __must_check_unlikely(({  \
  __typeof__(a) a__ = (a);                 \
  __typeof__(b) b__ = (b);                 \
  __typeof__(dst) dst__ = (dst);           \
  (void) (&a__ == &b__);                   \
  (void) (&a__ == dst__);                  \
  __builtin_sub_overflow(a__, b__, dst__); \
}))

#define check_mul_overflow(a, b, dst) __must_check_unlikely(({  \
  __typeof__(a) a__ = (a);                 \
  __typeof__(b) b__ = (b);                 \
  __typeof__(dst) dst__ = (dst);           \
  (void) (&a__ == &b__);                   \
  (void) (&a__ == dst__);                  \
  __builtin_mul_overflow(a__, b__, dst__); \
}))

// bool would_add_overflow(anyint a, anyint b)
#if __has_builtin(__builtin_add_overflow_p)
  #define would_add_overflow(a, b) \
    __builtin_add_overflow_p((a), (b), (__typeof__((a) + (b)))0)
#elif __has_builtin(__builtin_add_overflow)
  #define would_add_overflow(a, b) ({ \
    __typeof__((a) + (b)) tmp__; \
    __builtin_add_overflow((a), (b), &tmp__); \
  })
#else
  // best effort (triggers ubsan if enabled)
  #define would_add_overflow(a, b) ({ \
    __typeof__((a) + (b)) tmp__ = (i64)(a) + (i64)(b); \
    (u64)tmp__ < (u64)(a);
  })
#endif

//—————————————————————————————————————————————————————————————————————————————————————
// error codes

typedef int err_t;
enum err_ {
  ErrOk           =   0, // no error
  ErrInvalid      =  -1, // invalid data or argument
  ErrSysOp        =  -2, // invalid syscall op or syscall op data
  ErrBadfd        =  -3, // invalid file descriptor
  ErrBadName      =  -4, // invalid or misformed name
  ErrNotFound     =  -5, // resource not found
  ErrNameTooLong  =  -6, // name too long
  ErrCanceled     =  -7, // operation canceled
  ErrNotSupported =  -8, // not supported
  ErrExists       =  -9, // already exists
  ErrEnd          = -10, // end of resource
  ErrAccess       = -11, // permission denied
  ErrNoMem        = -12, // cannot allocate memory
  ErrMFault       = -13, // bad memory address
  ErrOverflow     = -14, // value too large
};

EXTERN_C err_t err_errno(); // current errno value
EXTERN_C err_t err_errnox(int errnoval);
EXTERN_C const char* err_str(err_t);

//—————————————————————————————————————————————————————————————————————————————————————
// builtin functions
//
// void* memset(void* p, int c, usize n);
// void* memcpy(void* restrict dst, const void* restrict src, usize n);
// void* memmove(void* dest, const void* src, usize n);
// int memcmp(const void* l, const void* r, usize n);
// usize strlen(const char* s);
// int strcmp(const char* l, const char* r);

#define memset __builtin_memset
#define memcpy __builtin_memcpy
#define memmove __builtin_memmove
#define memcmp __builtin_memcmp
#define strlen __builtin_strlen
#define strcmp __builtin_strcmp
#define strncmp __builtin_strncmp

// void __builtin_unreachable()
#if __has_builtin(__builtin_unreachable)
  #define UNREACHABLE __builtin_unreachable()
#elif __has_builtin(__builtin_trap)
  #define UNREACHABLE __builtin_trap
#else
  #define UNREACHABLE abort()
#endif


//—————————————————————————————————————————————————————————————————————————————————————
// memory

typedef struct {
  void* nullable p;    // start address
  usize          size; // size in bytes
} mem_t;

typedef struct {
  union {
    const void* nullable p;
    const u8*   nullable bytes;
    const char* nullable chars;
  };
  usize len;
} slice_t;

#define MEM(p, size)  ((mem_t){ (p), (size) })

// MEM_FMT is used for printf formatting of a mem_t
#define MEM_FMT          "{%p … %p %zu}"
#define MEM_FMT_ARGS(m)  (m).p, ((m).p+(m).size), (m).size

// MEM_POISON constants are non-NULL addresses which will result in page faults.
// Values match those of Linux.
#define MEM_POISON1 ((void*)0x100)
#define MEM_POISON2 ((void*)0x122)

// mem_is_null returns true if m.p==NULL or m.size==0.
inline static bool mem_is_null(mem_t m) {
  return (u8)!m.p | (u8)!m.size;
}
// mem_is_overflow returns true if m.p+m.size overflows
inline static bool mem_is_overflow(mem_t m) {
  return would_add_overflow((uintptr)m.p, (uintptr)m.size);
}
// mem_is_valid returns true if m is not null and size does not overflow p
inline static bool mem_is_valid(mem_t m) {
  return (u8)!mem_is_null(m) & (u8)!mem_is_overflow(m);
}

// mem_fill sets every byte of m to b
inline static void mem_fill(mem_t m, u8 b) {
  memset(m.p, b, m.size);
}

// mem_slice returns a slice of memory
// 1. mem_slice(const mem_t mem)
// 2. mem_slice(const mem_t mem, usize start, usize len)
#define mem_slice(...) __VARG_DISP(_mem_slice,__VA_ARGS__)
inline static slice_t _mem_slice1(const mem_t mem) {
  return *(slice_t*)&mem;
}
inline static slice_t _mem_slice3(const mem_t mem, usize start, usize len) {
  assert(start+len <= mem.size);
  return (slice_t){ .p = (u8*)mem.p + start, .len = mem.size - len };
}

// memalloc_t is a heap memory allocator
typedef struct memalloc* memalloc_t;

// mem_alloc allocates a region of at least size bytes. Returns .p=NULL on failure.
static mem_t mem_alloc(memalloc_t, usize size);

// mem_alloc_zeroed allocates a region of at least size bytes, initialized to zero.
// Returns .p=NULL on failure.
static mem_t mem_alloc_zeroed(memalloc_t, usize size);

// mem_alloctv allocates a zero-initialized array of count elements of size elemsize
void* nullable mem_allocv(memalloc_t, usize count, usize elemsize);

// mem_alloct allocates a zero-initialized element of type T
// T* nullable mem_alloct(memalloc_t, TYPE T)
#define mem_alloct(ma, T)  ( (T* nullable)mem_alloc_zeroed((ma), sizeof(T)).p )

// mem_alloctv allocates a zero-initialized array of count T elements
// T* nullable mem_alloctv(memalloc_t, TYPE T, usize count)
#define mem_alloctv(ma, T, count)  (T* nullable)mem_allocv((ma), (count), sizeof(T))

// mem_resize grows or shrinks the size of an allocated memory region to newsize.
// If resizing fails, false is returned and the region is unchanged; it is still valid.
static bool mem_resize(memalloc_t, mem_t* m, usize newsize);

// mem_free frees a region.
// In safe mode:
// - calls panic() if m.p is invalid
// - sets m->p=NULL and m->size=0
static void mem_free(memalloc_t, mem_t* m);
static void mem_freex(memalloc_t, mem_t m); // does not zero m

// mem_freev frees an array allocated with mem_allocv
static void mem_freev(memalloc_t, void* array, usize count, usize elemsize);

// mem_freet frees an element of type T
// void mem_freet(memalloc_t, T* ptr)
#define mem_freet(ma, ptr)  mem_free2((ma), (ptr), sizeof(*(ptr)))

// mem_freetv frees an array allocated with mem_alloctv
#define mem_freetv(ma, array, count)  mem_freev((ma), (array), (count), sizeof(*(array)))

// utilities
char* nullable mem_strdup(memalloc_t, slice_t src, usize extracap);

// allocators
#define MEMALLOC_STORAGE_ZEROED 1 // flag to memalloc_bump: storage is zeroed
static memalloc_t memalloc_ctx(); // current contextual allocator
static memalloc_t memalloc_ctx_set(memalloc_t); // returns previous allocator
static memalloc_t memalloc_default(); // the default allocator
static memalloc_t memalloc_null(); // an allocator that always fails
memalloc_t memalloc_bump(void* storage, usize cap, int flags); // create bump allocator

// memalloc_scope_set saves the current contextual allocator on the stack
// and sets newma as the current contextual allocator.
// When the current lexical scope ends, the previous contextual allocator is restored.
//
// Example:
//   void foo() {
//     memalloc_scope_set(ma0);
//     // memalloc_ctx is ma0 here
//     bar();
//     // memalloc_ctx is ma0 here
//   }
//   void bar() {
//     // memalloc_ctx is ma0 here
//     memalloc_scope_set(ma1);
//     // memalloc_ctx is ma1 here
//     {
//       memalloc_scope_set(ma2);
//       // memalloc_ctx is ma2 here
//     } // memalloc_ctx is restored to ma1
//     // memalloc_ctx is ma1 here
//   } // memalloc_ctx is restored to ma0
//
// memalloc_scope_set can be called multiple times in a given lexical scope. e.g.
//   // memalloc_ctx is ma0 here
//   {
//     memalloc_scope_set(ma1)
//     // memalloc_ctx is ma1 here
//     memalloc_scope_set(ma2)
//     // memalloc_ctx is ma2 here
//   }
//   // memalloc_ctx is ma0 here
//
// void memalloc_scope_set(memalloc_t newma)
#define memalloc_scope_set(newma) \
  __attribute__((cleanup(_memalloc_scope_reset))) \
  UNUSED memalloc_t CONCAT(_tmp,__COUNTER__) = memalloc_ctx_set(newma)

// memalloc_scope declares a new scope that uses newma as the contextual
// allocator. When the scope ends, the previous contextual allocator is restored.
// Example:
//   void foo() {
//     memalloc_scope(ma1) {
//       // memalloc_ctx is ma1 here
//     } // memalloc_ctx is restored
//   }
// void memalloc_scope(memalloc_t newma)
#define memalloc_scope(newma) \
  for ( \
    __attribute__((cleanup(_memalloc_scope_reset))) \
      UNUSED memalloc_t _prevma = memalloc_ctx_set(newma);\
    _prevma != NULL; \
    memalloc_ctx_set(_prevma), _prevma->f = (void*)(uintptr)1, _prevma = NULL )


inline static slice_t slice_cstr(const char* cstr) {
  return (slice_t){ .chars = cstr, .len = strlen(cstr) };
}

// ——————————————————————————
// memory api impl

struct memalloc {
  bool (*f)(void* self, mem_t*, usize newsize, bool zeroed);
};

extern struct memalloc _memalloc_default;
inline static memalloc_t memalloc_default() {
  return &_memalloc_default;
}

extern struct memalloc _memalloc_null;
inline static memalloc_t memalloc_null() {
  return &_memalloc_null;
}

extern _Thread_local memalloc_t _memalloc_ctx;
void _memalloc_scope_reset(memalloc_t* prev);
inline static memalloc_t memalloc_ctx() { return _memalloc_ctx; }
inline static memalloc_t memalloc_ctx_set(memalloc_t newma) {
  memalloc_t prevma = _memalloc_ctx;
  _memalloc_ctx = newma;
  //dlog("memalloc_ctx_set %p -> %p", prevma, newma);
  return prevma;
}

WARN_UNUSED_RESULT inline static mem_t mem_alloc(memalloc_t ma, usize size) {
  mem_t m = {0};
  ma->f(ma, &m, size, false);
  return m;
}

WARN_UNUSED_RESULT inline static mem_t mem_alloc_zeroed(memalloc_t ma, usize size) {
  mem_t m = {0};
  ma->f(ma, &m, size, true);
  return m;
}

WARN_UNUSED_RESULT inline static bool mem_resize(memalloc_t ma, mem_t* m, usize size) {
  return ma->f(ma, m, size, false);
}

inline static void mem_free(memalloc_t ma, mem_t* m) {
  ma->f(ma, m, 0, false);
}

inline static void mem_freex(memalloc_t ma, mem_t m) {
  ma->f(ma, &m, 0, false);
}

inline static void mem_free2(memalloc_t ma, void* p, usize size) {
  mem_t m = { .p = p, .size = size };
  ma->f(ma, &m, 0, false);
}

inline static void mem_freev(memalloc_t ma, void* array, usize count, usize elemsize) {
  assert_no_mul_overflow(count, elemsize);
  mem_free2(ma, array, count * elemsize);
}

//—————————————————————————————————————————————————————————————————————————————————————
// string functions

#define UTF8_SELF  0x80 // UTF-8 "self" byte constant

// character classifiers
#define isdigit(c)    ( ((u32)(c) - '0') < 10 )                 /* 0-9 */
#define isalpha(c)    ( ((u32)(c) | 32) - 'a' < 26 )            /* A-Za-z */
#define isalnum(c)    ( isdigit(c) || isalpha(c) )              /* 0-9A-Za-z */
#define isupper(c)    ( ((u32)(c) - 'A') < 26 )                 /* A-Z */
#define islower(c)    ( ((u32)(c) - 'a') < 26 )                 /* a-z */
#define isprint(c)    ( ((u32)(c) - 0x20) < 0x5f )              /* SP-~ */
#define isgraph(c)    ( ((u32)(c) - 0x21) < 0x5e )              /* !-~ */
#define isspace(c)    ( (c) == ' ' || (u32)(c) - '\t' < 5 )     /* SP, \{tnvfr} */
#define ishexdigit(c) ( isdigit(c) || ((u32)c | 32) - 'a' < 6 ) /* 0-9A-Fa-f */

#define ascii_tolower(c) ( (c) | 0x20 )

isize slastindexofn(const char* s, usize len, char c);
isize sindexof(const char* s, char c);
isize slastindexof(const char* s, char c);

// strim_begin returns offset into s past any leading trimc characters.
// e.g. strim_begin("  hello", 7, ' ') => "hello"
const char* strim_begin(const char* s, usize len, char trimc);

// strim_end returns the length of s without any trailing trimc characters.
// e.g. strim_end("hello  ", 7, ' ') => 5
usize strim_end(const char* s, usize len, char trimc);

usize sfmtu64(char* buf, u64 v, u32 base);

// // mutable string (null terminated)
// typedef struct {
//   char* nullable p; // only NULL when str_make fails to allocate memory
//   u32 cap, len;
// } str_t;
// str_t str_make(memalloc_t ma, slice_t src);

//—————————————————————————————————————————————————————————————————————————————————————
// files

err_t mmap_file(const char* filename, mem_t* data_out);
err_t mmap_unmap(mem_t);
err_t writefile(const char* filename, u32 mode, slice_t data);
err_t fs_mkdirs(const char* path, usize pathlen, int perms);

//—————————————————————————————————————————————————————————————————————————————————————
// promise

typedef struct promise {
  pid_t pid;
  err_t err;
} promise_t;

void promise_open(promise_t* p, pid_t pid);
void promise_open_done(promise_t* p, err_t result_err);
void promise_close(promise_t* p);
err_t promise_await(promise_t* p);
inline static bool promise_isresolved(const promise_t* p) { return p->pid == 0; }

ASSUME_NONNULL_END
