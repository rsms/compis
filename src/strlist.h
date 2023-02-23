// growable argv-compatible string array with efficient memory usage
// SPDX-License-Identifier: Apache-2.0
//
// Example:
//   strlist_t cflags = strlist_make(c->ma,
//     "-v",
//     "-o", "file");
//   strlist_addf(&cflags, "--level=%d", 12);
//   char* const* v = strlist_array(&cflags);
//   for (int i = 0; i < cflags.len; i++)
//     dlog("cflags[%d] = %s", i, v[i]);
//
#pragma once
#include "buf.h"
ASSUME_NONNULL_BEGIN

typedef struct {
  buf_t          buf;
  int            len;
  bool           ok;  // false if memory allocation failed or overflow occurred
  void* nullable _ap; // array of pointers into buf.p (char**); NULL until requested
} strlist_t;

// strlist_t strlist_make(memalloc_t ma, const char* arg...)
#define strlist_make(...) __VARG_DISP(_strlist_make,__VA_ARGS__)
void strlist_dispose(strlist_t* a);

void strlist_addf(strlist_t* a, const char* fmt, ...) ATTR_FORMAT(printf, 2, 3);
void strlist_add_list(strlist_t* a, const strlist_t* b);
void strlist_add_array(strlist_t* a, const char*const* src, usize len);
void strlist_add_raw(strlist_t* a, const char* src, usize len, int count);

inline static strlist_t strlist_save(strlist_t* a) { return *a; }
void strlist_restore(strlist_t* a, const strlist_t snapshot);

// void strlist_add(memalloc_t ma, const char* arg...)
#define strlist_add(...) __VARG_DISP(_strlist_add,__VA_ARGS__)

// strlist_array returns an array of pointers to null-terminated strings.
// The count of returned pointers is a->len.
char* const* strlist_array(strlist_t* a);

// strlist_add implementation
void _strlist_add2(strlist_t* a, const char* cstr);
void _strlist_adda(strlist_t* a, int count, ...);
#define _strlist_add3(a, args...) _strlist_adda(a, 2, args)
#define _strlist_add4(a, args...) _strlist_adda(a, 3, args)
#define _strlist_add5(a, args...) _strlist_adda(a, 4, args)
#define _strlist_add6(a, args...) _strlist_adda(a, 5, args)
#define _strlist_add7(a, args...) _strlist_adda(a, 6, args)
#define _strlist_add8(a, args...) _strlist_adda(a, 7, args)
#define _strlist_add9(a, args...) _strlist_adda(a, 8, args)
#define _strlist_add10(a, args...) _strlist_adda(a, 9, args)

// strlist_make implementation
strlist_t _strlist_make1(memalloc_t ma);
strlist_t _strlist_make2(memalloc_t ma, const char* a);
strlist_t _strlist_makea(memalloc_t ma, int count, ...);
#define _strlist_make3(ma, args...) _strlist_makea(ma, 2, args)
#define _strlist_make4(ma, args...) _strlist_makea(ma, 3, args)
#define _strlist_make5(ma, args...) _strlist_makea(ma, 4, args)
#define _strlist_make6(ma, args...) _strlist_makea(ma, 5, args)
#define _strlist_make7(ma, args...) _strlist_makea(ma, 6, args)
#define _strlist_make8(ma, args...) _strlist_makea(ma, 7, args)
#define _strlist_make9(ma, args...) _strlist_makea(ma, 8, args)
#define _strlist_make10(ma, args...) _strlist_makea(ma, 9, args)

ASSUME_NONNULL_END
