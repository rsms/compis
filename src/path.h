// file paths
// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "str.h"
#include <alloca.h>
#include <libgen.h>
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
#define PATH_SEP       PATH_SEPARATOR
#define PATH_SEP_STR   PATH_SEPARATOR_STR
#define PATH_DELIM     PATH_DELIMITER
#define PATH_DELIM_STR PATH_DELIMITER_STR

// path_dir returns path without the last component.
// E.g. "foo/bar/baz.x" => "foo/bar", "/a//b//" => "/a//b", "/lol" => "/"
str_t path_dir(const char* path) WARN_UNUSED_RESULT;

// path_dir writes all but the last path component to buf.
// Returns bytes written to buf as if bufcap was infinite.
usize path_dir_buf(char* buf, usize bufcap, const char* path);

// path_base returns a pointer to the last path element. E.g. "foo/bar/baz.x" => "baz.x"
// If the path is empty, returns "".
// If the path consists entirely of slashes, returns "/".
const char* path_base(const char* path);

// path_ext returns a pointer to the filename extension, or end of path if none.
// e.g. path_ext("a.b/c.d") => ".d", path_ext("a.b/c") => ""
const char* path_ext(const char* path);

// path_clean resolves parent paths ("..") and eliminates redundant "/" and "./",
// reducing 'path' to a clean, canonical form. E.g. "a/b/../c//./d" => "a/c/d"
usize path_cleanx(char* buf, usize bufcap, const char* path, usize pathlen);
usize path_cleanx_posix(char* buf, usize bufcap, const char* path, usize pathlen);
bool path_clean(str_t* path); // uses system PATH_SEP
bool path_clean_posix(str_t* path); // uses '/'
inline static char* path_clean_cstr(char* path) {
  usize len = strlen(path);
  return path_cleanx(path, len + 1, path, len), path;
}

// path_parselist parses a PATH_DELIMITER separated list. Each path is path_clean'ed.
// Ignores empty entries, e.g. "a:b::c" yields ["a","b","c"] (not ["a","b","","c"]).
// Returns a NULL-terminated array (or NULL if memory allocation failed.)
char** nullable path_parselist(memalloc_t ma, const char* pathlist);

// str_t path_join(const char* path ...)
// joins two or more path components together and calls path_clean on the result.
#define path_join(paths...) __VARG_DISP(_path_join, paths)
#define _path_join1(paths...) _path_join(1,paths)
#define _path_join2(paths...) _path_join(2,paths)
#define _path_join3(paths...) _path_join(3,paths)
#define _path_join4(paths...) _path_join(4,paths)
#define _path_join5(paths...) _path_join(5,paths)
#define _path_join6(paths...) _path_join(6,paths)
#define _path_join7(paths...) _path_join(7,paths)
#define _path_join8(paths...) _path_join(8,paths)
#define _path_join9(paths...) _path_join(9,paths)
str_t _path_join(usize count, ...) WARN_UNUSED_RESULT;
str_t path_joinv(usize count, va_list ap) WARN_UNUSED_RESULT;

// path_isabs returns true if path is absolute
inline static bool path_isabs(const char* path) { return *path == PATH_SEPARATOR; }

// path_abs resolves path relative to cwd and calls path_clean on the result.
// Note: It does NOT resolve symlinks (use realpath for that); path need no exist.
str_t path_abs(const char* path) WARN_UNUSED_RESULT;

// path_makeabs works like path_abs, but updates the string in place.
// Returns false if memory allocation failed.
bool path_makeabs(str_t* path);

// path_cwd returns the current working directory
str_t path_cwd() WARN_UNUSED_RESULT;

// relpath returns the path relative to the initial current working directory
const char* relpath(const char* path);
void relpath_init();

// path_isrooted returns true if path is or equal to or under dir.
// E.g. ("/foo/bar/cat", "/foo/bar") => true
// E.g. ("/foo/bar",     "/foo/bar") => true
// E.g. ("/foo",         "/foo/bar") => false
// E.g. ("/foo/bars",    "/foo/bar") => false
bool path_isrooted(slice_t path, slice_t dir);

// path_dir_alloca allocates space on stack and calls path_dir.
// char* path_dir_alloca(const char* path)
#define path_dir_alloca(path) ({ \
  const char* p__ = (path); usize z__ = strlen(p__) + 1lu; \
  /* +(z__==1) for "" => "." */ \
  char* s__ = safechecknotnull(alloca(z__ + (z__ == 1))); \
  path_dir_buf(s__, z__ + (z__ == 1), p__); \
  s__; \
})

// path_join_alloca allocates space on stack and joins two or more paths together.
// char* path_join_alloca(const char* path1 ...)
#define path_join_alloca(args...) __VARG_DISP(_path_join_alloca, args)
#define _path_join_alloca1(p1) ({ \
  const char* p1__ = (p1); usize z1__ = strlen(p1__); \
  char* s__ = safechecknotnull(alloca(z1__ + 1)); \
  memcpy(s__, p1__, z1__); \
  s__[z1__] = 0; \
  path_cleanx(s__, z1__ + 1, s__, z1__); \
  s__; \
})
#define _path_join_alloca2(p1, p2) ({ \
  const char* p1__ = (p1); usize z1__ = strlen(p1__); \
  const char* p2__ = (p2); usize z2__ = strlen(p2__); \
  char* s__ = safechecknotnull(alloca(z1__ + 1 + z2__ + 1)); \
  memcpy(s__, p1__, z1__); \
  s__[z1__] = '/'; \
  memcpy(&s__[z1__+1], p2__, z2__); \
  s__[z1__+1+z2__] = 0; \
  path_cleanx(s__, z1__ + 1 + z2__ + 1, s__, z1__ + 1 + z2__); \
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
  path_cleanx(s__, z1__+1+z2__+1+z3__+1, s__, z1__+1+z2__+1+z3__); \
  s__; \
})

ASSUME_NONNULL_END
