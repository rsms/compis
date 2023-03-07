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
  if (initcwd_len > 1)
    initcwd[initcwd_len++] = '/';
}


const char* relpath(const char* path) {
  if (*path == 0 || *path != '/')
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


usize path_dirlen(const char* filename, usize len) {
  isize i = slastindexofn(filename, len, PATH_SEPARATOR);
  return strim_end(filename, (usize)MAX(0,i), PATH_SEPARATOR);
}


usize path_dir(char* buf, usize bufcap, const char* path) {
  // examples:
  //   "a/b/c"    => "a/b"
  //   "a/b//c//" => "a/b"
  //   "a"        => "."
  //   "a//"      => "."
  //   ""         => "."
  //   "/a"       => "/"
  //   "/"        => "/"
  //   "/////"    => "/"
  char singlec;
  if (!path || *path == 0) {
    singlec = '.';
    goto singlechar;
  }
  usize i = strlen(path) - 1;
  // trim away trailing separators, e.g. "a/b//c//" => "a/b//c" (or "//" => "/")
  for (; path[i] == PATH_SEPARATOR; i--) {
    if (i == 0) {
      singlec = PATH_SEPARATOR;
      goto singlechar;
    }
  }
  // skip last component, e.g. "a/b//c" => "a/b//" (or "a" => ".")
  for (; path[i] != PATH_SEPARATOR; i--) {
    if (i == 0) {
      singlec = '.';
      goto singlechar;
    }
  }
  // trim away trailing separators, e.g. "a/b//" => "a/b" (or "/a/" => "/")
  for (; path[i] == PATH_SEPARATOR; i--) {
    if (i == 0) {
      singlec = PATH_SEPARATOR;
      goto singlechar;
    }
  }
  usize len = MIN(bufcap - 1, i + 1);
  if (len > 0) {
    memcpy(buf, path, len);
    buf[len] = 0;
  }
  return i + 1;
singlechar:
  if (bufcap > 1 && singlec) *buf++ = singlec;
  if (bufcap > 0) *buf = 0;
  return (usize)(singlec != 0);
}


char* nullable path_dir_m(memalloc_t ma, const char* path) {
  usize cap = path_dirlen(path, strlen(path)) + 1;
  char* buf = mem_alloc(ma, cap).p;
  if (buf)
    path_dir(buf, cap, path);
  return buf;
}


const char* path_base(const char* path) {
  if (path[0] == 0)
    return path;
  usize len = strlen(path);
  const char* p = &path[len];
  for (; p != path && *(p-1) != PATH_SEPARATOR; )
    p--;
  return p;
}


const char* path_ext(const char* path) {
  path = path_base(path);
  const char* p = strrchr(path, '.');
  return p && p > path ? p : path+strlen(path);
}


usize path_cleanx(char* buf, usize bufcap, const char* path, usize len) {
  usize r = 0;      // read offset
  usize w = 0;      // write offset
  usize wl = 0;     // logical bytes written
  usize dotdot = 0; // w offset of most recent ".."

  #define DST_APPEND(c) ( buf[w] = c, w += (usize)(w < (bufcap-1)), wl++ )
  #define IS_SEP(c)     ((c) == PATH_SEPARATOR)

  if (len == 0) {
    DST_APPEND('.');
    goto end;
  }

  bool rooted = IS_SEP(path[0]);
  if (rooted) {
    DST_APPEND(PATH_SEPARATOR);
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
        while (w > dotdot && !IS_SEP(buf[w]))
          w--;
      } else if (!rooted) {
        // cannot backtrack, but not rooted, so append ".."
        if (w > 0)
          DST_APPEND(PATH_SEPARATOR);
        DST_APPEND('.');
        DST_APPEND('.');
        dotdot = w;
      }
    } else {
      // actual path component; add slash if needed
      if ((rooted && w != 1) || (!rooted && w != 0))
        DST_APPEND(PATH_SEPARATOR);
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


usize path_join(char* dst, usize dstcap, const char* path1, const char* path2) {
  usize len = (usize)snprintf(dst, dstcap, "%s/%s", path1, path2);
  return path_cleanx(dst, dstcap, dst, MIN(len, dstcap - 1));
}


char* nullable path_join_m(memalloc_t ma, const char* path1, const char* path2) {
  mem_t m = mem_alloc(ma, strlen(path1) + 1 + strlen(path2) + 1);
  if (m.p)
    path_join(m.p, m.size, path1, path2);
  return m.p;
}


char* nullable path_abs(memalloc_t ma, const char* path) {
  if (path_isabs(path)) {
    usize pathlen = strlen(path);
    mem_t m = mem_alloc(ma, pathlen + 1);
    if (m.p)
      path_cleanx(m.p, m.size, path, pathlen);
    return m.p;
  } else {
    char cwd[PATH_MAX];
    safechecknotnull(getcwd(cwd, sizeof(cwd)));
    return path_join_m(ma, cwd, path);
  }
}
