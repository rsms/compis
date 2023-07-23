// SPDX-License-Identifier: Apache-2.0
#include "colib.h"
#include "path.h"

#include <unistd.h>
#include <string.h>


static char initcwd[PATH_MAX]; // note: NOT null terminated
static usize initcwd_len = 0;


void relpath_init() {
  getcwd(initcwd, sizeof(initcwd));
  initcwd_len = strlen(initcwd);
  initcwd[initcwd_len++] = PATH_SEP;
}


const char* relpath(const char* path) {
  if (*path == 0 || *path != PATH_SEP)
    return path;
  usize len = strlen(path);
  if (len == initcwd_len-1) {
    if (memcmp(path, initcwd, len) == 0)
      return ".";
  } else if (len >= initcwd_len) {
    if (memcmp(path, initcwd, initcwd_len) == 0)
      return path + initcwd_len;
  }
  return path;
}


usize path_dir_len(const char* path, usize len) {
  // special case of empty string: we can't even say "/ is the dir"
  if (len == 0)
    return 0;

  const char* end = path + (len - 1);

  // skip past trailing slashes, e.g. "/a/b//" => "/a/b"
  while (*end == PATH_SEP && end != path)
    end--;

  // if path is only slashes, the dir is the root
  if (end == path)
    return 1;

  // find next slash
  const char* end2 = end;
  for (;;) {
    end2--;
    if (end2 == path) // root
      return (*path == PATH_SEP); // 1 for "/", 0 for ""
    if (*end2 == PATH_SEP)
      return (usize)(uintptr)(end2 - path);
  }
  UNREACHABLE;
}


static usize path_dir_len1(const char* path, char* singlecp) {
  // examples:
  //   "a/b/c"    => "a/b"
  //   "a/b//c//" => "a/b"
  //   "a"        => "."
  //   "a//"      => "."
  //   ""         => "."
  //   "/a"       => "/"
  //   "/"        => "/"
  //   "/////"    => "/"
  usize pathlen = strlen(path);
  if (pathlen == 0) {
    *singlecp = '.';
    return 1;
  }
  usize i = pathlen - 1;
  // trim away trailing separators, e.g. "a/b//c//" => "a/b//c" (or "//" => "/")
  for (; path[i] == PATH_SEPARATOR; i--) {
    if (i == 0) {
      *singlecp = PATH_SEPARATOR;
      return 1;
    }
  }
  // skip last component, e.g. "a/b//c" => "a/b//" (or "a" => ".")
  for (; path[i] != PATH_SEPARATOR; i--) {
    if (i == 0) {
      *singlecp = '.';
      return 1;
    }
  }
  // trim away trailing separators, e.g. "a/b//" => "a/b" (or "/a/" => "/")
  for (; path[i] == PATH_SEPARATOR; i--) {
    if (i == 0) {
      *singlecp = PATH_SEPARATOR;
      return 1;
    }
  }
  i++;
  if (i == 1)
    *singlecp = *path;
  return i;
}


usize path_dir_buf(char* buf, usize bufcap, const char* path) {
  char singlec;
  usize dirlen = path_dir_len1(path, &singlec);
  if (dirlen == 1) {
    if (bufcap > 1 && singlec) *buf++ = singlec;
    if (bufcap > 0) *buf = 0;
    return (usize)(singlec != 0);
  }
  usize copylen = MIN(bufcap - 1, dirlen);
  if (copylen > 0) {
    memcpy(buf, path, copylen);
    buf[copylen] = 0;
  }
  return dirlen;
}


str_t path_dir(const char* path) {
  char singlec;
  usize dirlen = path_dir_len1(path, &singlec);
  assert(dirlen > 0);
  if (dirlen == 1)
    path = &singlec;
  return str_makelen(path, dirlen);
}


const char* path_basen(const char* path, usize* lenp) {
  if (*lenp == 0)
    return path;
  const char* p = path + (*lenp - 1);
  // skip trailing separators, e.g. "/foo/bar/" => "bar/"
  while (*p == PATH_SEP && p != path)
    p--;
  while (*p != PATH_SEP && p != path)
    p--;
  if (p == path) {
    // no base; e.g. "/foo", "foo/", "foo" or "/"
    *lenp = 0;
    return path;
  }
  p++; // "undo" last PATH_SEP
  *lenp -= (usize)(uintptr)(p - path);
  return p;
}


const char* path_base_cstr(const char* path) {
  if (*path == 0)
    return path;
  const char* p = path + strlen(path) - 1;
  // skip trailing separators
  while (*p == PATH_SEP && p != path)
    p--;
  while (*p != PATH_SEP && p != path)
    p--;
  return p == path ? p : p + 1;
}


const char* path_ext_cstr(const char* path) {
  path = path_base_cstr(path);
  const char* p = strrchr(path, '.');
  return p && p > path ? p : path+strlen(path);
}


usize path_common_dirname(const char*const pathv[], usize pathc) {
  if (pathc == 0)
    return 0;

  if (pathc == 1)
    return path_dir_len(pathv[0], strlen(pathv[0]));

  // search left to right, char by char.
  // e.g. with input ["/foo/bar", "/foo/baz"]
  // 1. find the first differing char:
  //   / → /f → /fo → /foo → /foo/ → /foo/b → /foo/ba → /foo/bar differs
  //   / → /f → /fo → /foo → /foo/ → /foo/b → /foo/ba → /foo/baz differs
  //   ~    ~     ~      ~       ~        ~         ~          ~
  // 2. backtrack to the previous slash
  //   /foo/ ← /foo/b ← /foo/ba
  //       ~        ~         ~
  for (usize pos = 0; ; pos++) {
    for (usize i = 0; i < pathc; i++) {
      if (pathv[i][pos] != 0 && pathv[i][pos] == pathv[0][pos])
        continue;
      // backtrack to previous slash
      while (pos > 0 && pathv[0][--pos] != PATH_SEP) {}
      return pos;
    }
  }
  UNREACHABLE;
}


usize _path_clean(char* buf, usize bufcap, const char* path, usize len, char pathsep) {
  usize r = 0;      // read offset
  usize w = 0;      // write offset
  usize wl = 0;     // logical bytes written
  usize dotdot = 0; // w offset of most recent ".."

  #define DST_APPEND(c) ( buf[w] = c, w += (usize)(w < (bufcap-1)), wl++ )
  #define IS_SEP(c)     ((c) == pathsep)

  if (len == 0) {
    DST_APPEND('.');
    goto end;
  }

  bool rooted = IS_SEP(path[0]);
  if (rooted) {
    DST_APPEND(pathsep);
    r = 1;
    dotdot++;
  }

  while (r < len) {
    if (IS_SEP(path[r]) || (path[r] == '.' && (r+1 == len || IS_SEP(path[r+1])))) {
      // "/" or "."
      r++;
    } else if (path[r] == '.' && path[r+1] == '.' && (r+2 == len || IS_SEP(path[r+2]))) {
      // ".."
      r += 2;
      if (w > dotdot) {
        // can backtrack
        w--;
        wl--;
        while (w > dotdot && !IS_SEP(buf[w])) {
          w--;
          wl--;
        }
      } else if (!rooted) {
        // cannot backtrack, but not rooted, so append ".."
        if (w > 0)
          DST_APPEND(pathsep);
        DST_APPEND('.');
        DST_APPEND('.');
        dotdot = w;
      }
    } else {
      // actual path component; add slash if needed
      if ((rooted && w != 1) || (!rooted && w != 0))
        DST_APPEND(pathsep);
      // copy
      for (; r < len && !IS_SEP(path[r]); r++)
        DST_APPEND(path[r]);
    }
  }

  if (w == 0) // "" => "."
    DST_APPEND('.');

  #undef DST_APPEND
  #undef IS_SEP

end:
  buf[w] = 0;
  return wl;
}


usize path_cleanx(char* buf, usize bufcap, const char* path, usize len) {
  return _path_clean(buf, bufcap, path, len, PATH_SEP);
}


usize path_cleanx_posix(char* buf, usize bufcap, const char* path, usize len) {
  return _path_clean(buf, bufcap, path, len, '/');
}


bool path_clean(str_t* path) {
  if UNLIKELY(path->len == 0)
    return str_push(path, '.');
  path->len = _path_clean(path->p, path->cap, path->p, path->len, PATH_SEP);
  return true;
}


bool path_clean_posix(str_t* path) {
  if UNLIKELY(path->len == 0)
    return str_push(path, '.');
  path->len = _path_clean(path->p, path->cap, path->p, path->len, '/');
  return true;
}


str_t path_joinv(usize count, va_list ap) {
  str_t s = {0};
  if LIKELY(str_appendv(&s, PATH_SEP, count, ap)) {
    s.len = path_cleanx(s.p, s.cap, s.p, s.len);
    if UNLIKELY(s.len >= s.cap) {
      str_free(s);
      s = (str_t){0};
    }
  }
  return s;
}


str_t _path_join(usize count, ...) {
  va_list ap;
  va_start(ap, count);
  str_t s = path_joinv(count, ap);
  va_end(ap);
  return s;
}


str_t path_abs(const char* path) {
  if (path_isabs(path)) {
    str_t s = str_make(path);
    path_clean(&s);
    return s;
  }
  char cwd[PATH_MAX];
  return path_join(getcwd(cwd, sizeof(cwd)), path);
}


bool path_makeabs(str_t* path) {
  bool ok = path_clean(path);
  if (path_isabs(path->p))
    return ok;
  // note: initcwd[initcwd_len-1] is always PATH_SEP
  return str_prependlen(path, initcwd, initcwd_len);
}


str_t path_cwd() {
  char cwd[PATH_MAX];
  return str_make(getcwd(cwd, sizeof(cwd)));
}


char** nullable path_parselist(memalloc_t ma, const char* pathlist) {
  usize len = strlen(pathlist);
  usize count = 0;
  usize strcap = 0;

  for (usize start = 0, end = 0; ; end++) {
    if (end != len && pathlist[end] != PATH_DELIMITER)
      continue;
    if (end - start) {
      // dlog(">> '%.*s'", (int)(end - start), &pathlist[start]);
      strcap += (end - start) + 1; // +1 for NUL
      count++;
    }
    if (end == len)
      break;
    start = end + 1;
  }

  usize arraysize = sizeof(void*) * (count + 1);

  char** slist = mem_alloc(ma, arraysize + strcap).p;
  if UNLIKELY(slist == NULL)
    return NULL;

  char** slistp = slist;

  // strings are stored after pointer array
  char* strv = (void*)slist + arraysize;
  char* strp = strv;

  for (usize start = 0, end = 0; ; end++) {
    if (end != len && pathlist[end] != PATH_DELIMITER)
      continue;
    usize slen = end - start;
    if (slen) {
      *slistp++ = strp;
      memcpy(strp, &pathlist[start], slen);
      UNUSED usize n = path_cleanx(strp, slen, strp, slen);
      assert(n <= slen);
      strp += slen;
      *strp++ = 0;
    }
    if (end == len)
      break;
    start = end + 1;
  }

  *slistp = NULL;
  return slist;
}


bool path_isrooted(slice_t path, slice_t dir) {
  if (
    path.len == 0 || // e.g. ("", "/foo/bar")
    dir.len == 0 ||  // e.g. ("/foo/bar", "")
    path.len < dir.len || // e.g. ("/foo/ba", "/foo/bar")
    (path.len > dir.len && path.chars[dir.len] != PATH_SEP) // e.g. ("/a/bc", "/a/b")
  ) {
    return false;
  }
  return memcmp(path.p, dir.p, dir.len) == 0;
}
