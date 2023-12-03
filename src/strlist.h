// growable argv-compatible string array with efficient memory storage
// SPDX-License-Identifier: Apache-2.0
//
// Example:
//   strlist_t cflags = strlist_make(c->ma,
//     "-v",
//     "-o", "file");
//   strlist_addf(&cflags, "--level=%d", 12);
//   char* const* v = strlist_array(&cflags);
//   for (u32 i = 0; i < cflags.len; i++)
//     dlog("cflags[%d] = %s", i, v[i]);
//   strlist_dispose(&cflags);
//
#pragma once
#include "buf.h"
ASSUME_NONNULL_BEGIN

typedef struct {
  buf_t          buf;
  u32            len;
  bool           ok;  // false if memory allocation failed or overflow occurred
  void* nullable _ap; // array of pointers into buf.p (char**); NULL until requested
} strlist_t;

// strlist_t strlist_make(memalloc_t ma, const char* arg...)
#define strlist_make(...) __VARG_DISP(_strlist_make,__VA_ARGS__)
inline static void strlist_init(strlist_t* a, memalloc_t ma) {
  buf_init(&a->buf, ma);
  a->ok = true;
}
void strlist_dispose(strlist_t* a);

void strlist_addf(strlist_t* a, const char* fmt, ...) ATTR_FORMAT(printf, 2, 3);
void strlist_addv(strlist_t* a, const char* fmt, va_list);
void strlist_add_list(strlist_t* a, const strlist_t* b);
void strlist_add_array(strlist_t* a, const char*const* src, usize len);
void strlist_add_raw(strlist_t* a, const char* src, usize len, u32 count);
inline static void strlist_add_slice(strlist_t* a, slice_t strptrs) {
  strlist_add_array(a, strptrs.strings, strptrs.len);
}


inline static strlist_t strlist_save(strlist_t* a) { return *a; }
void strlist_restore(strlist_t* a, const strlist_t snapshot);

// void strlist_add(memalloc_t ma, const char* arg...)
#define strlist_add(...) __VARG_DISP(_strlist_add,__VA_ARGS__)

// add one item with explicit byte length.
// cstr[0:len] must not contain any NUL bytes or the behavior is undefined.
void strlist_addlen(strlist_t* a, const char* cstr, usize len);

// strlist_array returns an array of pointers to null-terminated strings.
// The count of returned pointers is a->len.
char* const* strlist_array(strlist_t* a);

// ———————————————————————————————————————————————————————————————————————————————
// implementation

inline static void _strlist_add1(strlist_t* _) {}
void _strlist_add2(strlist_t* a, const char* cstr);
void _strlist_adda(strlist_t* a, u32 count, ...);
#define _strlist_add3(a, args...) _strlist_adda(a, 2, args)
#define _strlist_add4(a, args...) _strlist_adda(a, 3, args)
#define _strlist_add5(a, args...) _strlist_adda(a, 4, args)
#define _strlist_add6(a, args...) _strlist_adda(a, 5, args)
#define _strlist_add7(a, args...) _strlist_adda(a, 6, args)
#define _strlist_add8(a, args...) _strlist_adda(a, 7, args)
#define _strlist_add9(a, args...) _strlist_adda(a, 8, args)
#define _strlist_add10(a, args...) _strlist_adda(a, 9, args)

strlist_t _strlist_make1(memalloc_t ma);
strlist_t _strlist_make2(memalloc_t ma, const char* a);
strlist_t _strlist_makea(memalloc_t ma, u32 count, ...);
#define _strlist_make3(ma, args...) _strlist_makea(ma, 2, args)
#define _strlist_make4(ma, args...) _strlist_makea(ma, 3, args)
#define _strlist_make5(ma, args...) _strlist_makea(ma, 4, args)
#define _strlist_make6(ma, args...) _strlist_makea(ma, 5, args)
#define _strlist_make7(ma, args...) _strlist_makea(ma, 6, args)
#define _strlist_make8(ma, args...) _strlist_makea(ma, 7, args)
#define _strlist_make9(ma, args...) _strlist_makea(ma, 8, args)
#define _strlist_make10(ma, args...) _strlist_makea(ma, 9, args)

ASSUME_NONNULL_END
