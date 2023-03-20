// null-terminated mutable byte strings, allocated in memalloc_default()
// SPDX-License-Identifier: Apache-2.0
#pragma once
ASSUME_NONNULL_BEGIN

#define STR_MEMALLOC memalloc_default()

typedef struct {
  char* nullable p;
  usize cap, len;
} str_t;

str_t str_makelen(const char* p, usize len);
static str_t str_make(const char* cstr);
inline void str_free(str_t);

// str_ensure_avail makes sure there is >= minavail free bytes at p+len
bool str_ensure_avail(str_t*, usize minavail);
inline static usize str_avail(str_t s) { return s.cap - (s.len + 1); }

// str_reserve allocates len bytes at s->p, plus a terminating null character,
// and then increments s->len by len.
char* nullable str_reserve(str_t*, usize len);

// str_push appends one character, plus a null terminator
bool str_push(str_t*, char c);

// bool str_append(str_t* s, const char* adds...)
// copies one or more c-strings to the end of s
#define str_append(s, adds...) __VARG_DISP(_str_append, s, adds)

// str_appendlen appends len characters from src to s, plus a terminating null char.
// Caution: Does not check if src contains null characters.
bool str_appendlen(str_t*, const char* src, usize len);

// str_appendv appends 'count' c-strings, optionally separated by 'glue'.
// Set glue=0 to join strings together without a glue character.
bool str_appendv(str_t*, char glue, usize count, va_list ap);

// str_appendstrings calls str_appendv with 'count' input strings
bool str_appendstrings(str_t* s, char glue, usize count, ...);

// str_slice returns a slice_t of a str_t
// 1. slice_t str_slice(const str_t s)
// 2. slice_t str_slice(const str_t s, usize start, usize len)
#define str_slice(...) __VARG_DISP(_str_slice,__VA_ARGS__)
inline static slice_t _str_slice1(const str_t s) {
  return (slice_t){ .p = s.p, .len = s.len };
}
inline static slice_t _str_slice3(const str_t s, usize start, usize len) {
  assert(start + len <= s.len);
  return (slice_t){ .p = s.p + start, .len = s.len - len };
}

//—————————————————————————————————————————————————————————————————————————————————————
// implementation

inline static str_t str_make(const char* cstr) {
  return str_makelen(cstr, strlen(cstr));
}

#define str_free(s) mem_free(STR_MEMALLOC, (mem_t*)&(s))

inline static bool _str_append2(str_t* s, const char* cstr) {
  return str_appendlen(s, cstr, strlen(cstr));
}
#define _str_append3(s, adds...) str_appendstrings(s, 0, 2, adds)
#define _str_append4(s, adds...) str_appendstrings(s, 0, 3, adds)
#define _str_append5(s, adds...) str_appendstrings(s, 0, 4, adds)
#define _str_append6(s, adds...) str_appendstrings(s, 0, 5, adds)
#define _str_append7(s, adds...) str_appendstrings(s, 0, 6, adds)
#define _str_append8(s, adds...) str_appendstrings(s, 0, 7, adds)
#define _str_append9(s, adds...) str_appendstrings(s, 0, 8, adds)

// #else
//   #define str_free _str_free
// #endif
// inline static void _str_free(str_t s) {
//   mem_t* m = (mem_t*)&s;
//   #if USIZE_MAX != U32_MAX
//     m->size = (usize)s.cap;
//   #endif
//   mem_free(STR_MEMALLOC, m);
// }

/*
// str_t is a mutable null-terminated string, always allocated in memalloc_default()
typedef struct {
  char* p[];
  u32 cap, len;
} str_t;

str_t* nullable str_makedata(const void* data, usize size);

inline static str_t* nullable str_make(const char* cstr) {
  return str_makedata(cstr, strlen(cstr));
}

void str_free(str_t* s);*/

ASSUME_NONNULL_END
