// file paths
// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <alloca.h>
ASSUME_NONNULL_BEGIN

#ifdef WIN32
  #define PATH_SEPARATOR     '\\'
  #define PATH_SEPARATOR_STR "\\"
  #define PATH_DELIMITER     ';'
  #define PATH_DELIMITER_STR ";"
#else
  #define PATH_SEPARATOR     '/'
  #define PATH_SEPARATOR_STR "/"
  #define PATH_DELIMITER     ':'
  #define PATH_DELIMITER_STR ":"
#endif

// path_diroffs returns the position of the end of the directory part of path.
// E.g. "foo/bar/baz" => 7, "foo/" => 3, "foo" => 0.
// Returns 0 if path does not contain a directory part.
usize path_dirlen(const char* path, usize len);

// path_base returns a pointer to the last path element. E.g. "foo/bar/baz.x" => "baz.x"
// If the path is empty, returns "".
// If the path consists entirely of slashes, returns "/".
const char* path_base(const char* path);

usize path_clean(char* restrict buf, usize bufcap, const char* restrict path);
usize path_cleann(char* restrict buf, usize bufcap, const char*restrict path, usize len);

char* nullable path_join(memalloc_t, const char* path1, const char* path2);
char* nullable path_joinslice(memalloc_t, slice_t path1, slice_t path2);

// dirname_alloca allocates space on stack and calls dirname_r.
// char* path_clean_alloca(const char* path)
#define dirname_alloca(path) ({ \
  const char* p__ = (path); usize z__ = strlen(p__); \
  char* s__ = safechecknotnull(alloca(z__ + 1)); \
  safechecknotnull(dirname_r(p__, s__)); \
  s__; \
})

// path_clean_alloca allocates space on stack and calls path_clean.
// char* path_clean_alloca(const char* path)
#define path_clean_alloca(path) ({ \
  const char* p__ = (path); usize z__ = strlen(p__); \
  char* s__ = safechecknotnull(alloca(z__ + 1)); \
  UNUSED usize n__ = path_clean(s__, z__ + 1, p__); \
  safecheck(n__ <= z__); \
  s__; \
})

// path_join_alloca allocates space on stack and joins two paths together.
// path_join_alloca does NOT call path_clean (like path_join does.)
//
// char* path_join_alloca(const char* path1, const char* path2)
// char* path_join_alloca(const char* path1, const char* path2, const char* path3)
#define path_join_alloca(args...) __VARG_DISP(_path_join_alloca, args)
#define _path_join_alloca2(p1, p2) ({ \
  const char* p1__ = (p1); usize z1__ = strlen(p1__); \
  const char* p2__ = (p2); usize z2__ = strlen(p2__); \
  char* s__ = safechecknotnull(alloca(z1__ + 1 + z2__ + 1)); \
  memcpy(s__, p1__, z1__); \
  s__[z1__] = '/'; \
  memcpy(&s__[z1__+1], p2__, z2__); \
  s__[z1__+1+z2__] = 0; \
  s__; \
})
#define _path_join_alloca3(p1, p2, p3) ({ \
  const char* p1__ = (p1); usize z1__ = strlen(p1__); \
  const char* p2__ = (p2); usize z2__ = strlen(p2__); \
  const char* p3__ = (p3); usize z3__ = strlen(p3__); \
  char* s__ = safechecknotnull(alloca(z1__ + 1 + z2__ + 1 + z3__ + 1)); \
  memcpy(s__, p1__, z1__); \
  s__[z1__] = '/'; \
  memcpy(&s__[z1__+1], p2__, z2__); \
  s__[z1__+1+z2__] = '/'; \
  memcpy(&s__[z1__+1+z2__+1], p3__, z3__); \
  s__[z1__+1+z2__+1+z3__] = 0; \
  s__; \
})

ASSUME_NONNULL_END
