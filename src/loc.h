// loc_t is a compact representation of a source location: file, line, column & width.
// Inspired by the Go compiler's xpos & lico. (loc_t)0 is invalid.
// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "thread.h"
#include "array.h"
ASSUME_NONNULL_BEGIN

typedef u64 loc_t;

typedef struct srcfile_ srcfile_t;

// locmap_t maps loc_t to srcfile_t.
// All locmap_ functions are thread safe.
typedef struct {
  array_type(const srcfile_t*) m; // {loc_t => srcfile_t*} (slot 0 is always NULL)
  rwmutex_t mu; // guards access to m
} locmap_t;

// origin_t describes the origin of diagnostic message (usually derived from loc_t)
typedef struct origin_t {
  const srcfile_t* nullable file;
  u32 line;      // 0 if unknown (if so, other fields below are invalid)
  u32 column;
  u32 width;     // >0 if it's a range (starting at line & column)
  u32 focus_col; // if >0, signifies important column at loc_line(loc)
} origin_t;


err_t locmap_init(locmap_t* lm);
void locmap_dispose(locmap_t* lm, memalloc_t);
void locmap_clear(locmap_t* lm);
u32 locmap_intern_srcfileid(locmap_t* lm, const srcfile_t*, memalloc_t);
u32 locmap_lookup_srcfileid(locmap_t* lm, const srcfile_t*); // 0 = not found
const srcfile_t* nullable locmap_srcfile(locmap_t* lm, u32 srcfileid);

static loc_t loc_make(u32 srcfileid, u32 line, u32 col, u32 width);

const srcfile_t* nullable loc_srcfile(loc_t p, locmap_t*);
static u32 loc_line(loc_t p);
static u32 loc_col(loc_t p);
static u32 loc_width(loc_t p);
static u32 loc_srcfileid(loc_t p); // key for locmap_t; 0 for pos without input

static loc_t loc_with_srcfileid(loc_t p, u32 srcfileid); // copy of p with srcfileid
static loc_t loc_with_line(loc_t p, u32 line);       // copy of p with specific line
static loc_t loc_with_col(loc_t p, u32 col);         // copy of p with specific col
static loc_t loc_with_width(loc_t p, u32 width);     // copy of p with specific width

static void loc_set_line(loc_t* p, u32 line);
static void loc_set_col(loc_t* p, u32 col);
static void loc_set_width(loc_t* p, u32 width);

// loc_adjuststart returns a copy of p with its start and width adjusted by deltacol
loc_t loc_adjuststart(loc_t p, i32 deltacol); // cannot overflow (clamped)

// loc_union returns a loc_t that covers the column extent of both a and b
loc_t loc_union(loc_t a, loc_t b); // a and b must be on the same line

static loc_t loc_min(loc_t a, loc_t b);
static loc_t loc_max(loc_t a, loc_t b);
inline static bool loc_isknown(loc_t p) { return !!(loc_srcfileid(p) | loc_line(p)); }

// p is {before,after} q in same input
inline static bool loc_isbefore(loc_t p, loc_t q) { return p < q; }
inline static bool loc_isafter(loc_t p, loc_t q) { return p > q; }

// loc_fmt appends "file:line:col" to buf (behaves like snprintf)
usize loc_fmt(loc_t p, char* buf, usize bufcap, locmap_t* lm);


// origin_make creates a origin_t
// 1. origin_make(locmap_t*, loc_t)
// 2. origin_make(locmap_t*, loc_t, u32 focus_col)
#define origin_make(...) __VARG_DISP(_origin_make,__VA_ARGS__)
#define _origin_make1 _origin_make2 // catch "too few arguments to function call"
origin_t _origin_make2(locmap_t* m, loc_t loc);
origin_t _origin_make3(locmap_t* m, loc_t loc, u32 focus_col);

origin_t origin_union(origin_t a, origin_t b);


//———————————————————————————————————————————————————————————————————————————————————————
// implementation

// Limits: files: 1048575, lines: 1048575, columns: 4095, width: 4095
// If this is too tight, we can either make lico 64b wide, or we can introduce a
// tiered encoding where we remove column information as line numbers grow bigger.
static const u64 _loc_widthBits     = 12;
static const u64 _loc_colBits       = 12;
static const u64 _loc_lineBits      = 20;
static const u64 _loc_srcfileidBits = 64 - _loc_lineBits - _loc_colBits - _loc_widthBits;

static const u64 _loc_srcfileidMax = (1llu << _loc_srcfileidBits) - 1;
static const u64 _loc_lineMax      = (1llu << _loc_lineBits) - 1;
static const u64 _loc_colMax       = (1llu << _loc_colBits) - 1;
static const u64 _loc_widthMax     = (1llu << _loc_widthBits) - 1;

static const u64 _loc_srcfileidShift =
  _loc_srcfileidBits + _loc_colBits + _loc_widthBits;
static const u64 _loc_lineShift = _loc_colBits + _loc_widthBits;
static const u64 _loc_colShift  = _loc_widthBits;


inline static loc_t loc_make_unchecked(u32 srcfileid, u32 line, u32 col, u32 width) {
  return (loc_t)( ((loc_t)srcfileid << _loc_srcfileidShift)
              | ((loc_t)line << _loc_lineShift)
              | ((loc_t)col << _loc_colShift)
              | width );
}
inline static loc_t loc_make(u32 srcfileid, u32 line, u32 col, u32 width) {
  return loc_make_unchecked(
    MIN(_loc_srcfileidMax, srcfileid),
    MIN(_loc_lineMax, line),
    MIN(_loc_colMax, col),
    MIN(_loc_widthMax, width));
}
inline static u32 loc_srcfileid(loc_t p) { return p >> _loc_srcfileidShift; }
inline static u32 loc_line(loc_t p)    { return (p >> _loc_lineShift) & _loc_lineMax; }
inline static u32 loc_col(loc_t p)     { return (p >> _loc_colShift) & _loc_colMax; }
inline static u32 loc_width(loc_t p)   { return p & _loc_widthMax; }

// TODO: improve the efficiency of these
inline static loc_t loc_with_srcfileid(loc_t p, u32 srcfileid) {
  return loc_make_unchecked(
    MIN(_loc_srcfileidMax, srcfileid), loc_line(p), loc_col(p), loc_width(p));
}
inline static loc_t loc_with_line(loc_t p, u32 line) {
  return loc_make_unchecked(
    loc_srcfileid(p), MIN(_loc_lineMax, line), loc_col(p), loc_width(p));
}
inline static loc_t loc_with_col(loc_t p, u32 col) {
  return loc_make_unchecked(
    loc_srcfileid(p), loc_line(p), MIN(_loc_colMax, col), loc_width(p));
}
inline static loc_t loc_with_width(loc_t p, u32 width) {
  return loc_make_unchecked(
    loc_srcfileid(p), loc_line(p), loc_col(p), MIN(_loc_widthMax, width));
}

inline static void loc_set_line(loc_t* p, u32 line) { *p = loc_with_line(*p, line); }
inline static void loc_set_col(loc_t* p, u32 col) { *p = loc_with_col(*p, col); }
inline static void loc_set_width(loc_t* p, u32 width) { *p = loc_with_width(*p, width); }

inline static loc_t loc_min(loc_t a, loc_t b) {
  // pos-1 causes (loc_t)0 to become the maximum value of loc_t,
  // effectively preferring >(loc_t)0 over (loc_t)0 here.
  return (b-1 < a-1) ? b : a;
}
inline static loc_t loc_max(loc_t a, loc_t b) {
  return (b > a) ? b : a;
}


ASSUME_NONNULL_END
