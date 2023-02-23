// SPDX-License-Identifier: Apache-2.0
#include "colib.h"
#include "path.h"


usize path_dirlen(const char* filename, usize len) {
  isize i = slastindexofn(filename, len, PATH_SEPARATOR);
  return strim_end(filename, (usize)MAX(0,i), PATH_SEPARATOR);
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


usize path_clean(char* restrict buf, usize bufcap, const char* restrict path) {
  return path_cleann(buf, bufcap, path, strlen(path));
}


usize path_cleann(
  char* restrict buf, usize bufcap, const char* restrict path, usize len)
{
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


char* nullable path_joinslice(memalloc_t ma, slice_t path1, slice_t path2) {
  usize size = path1.len + 1 + path2.len + 1;

  char* result = mem_strdup(ma, path1, 1 + path2.len);
  if UNLIKELY(!result)
    return NULL;

  char* stmp = mem_strdup(ma, path1, 1 + path2.len);
  if UNLIKELY(!stmp) {
    mem_freetv(ma, result, size);
    return NULL;
  }
  char* p = stmp + path1.len;
  *p++ = PATH_SEPARATOR;
  memcpy(p, path2.p, path2.len);
  p[path2.len] = 0;

  usize n = path_cleann(result, size, stmp, size-1);

  mem_freetv(ma, stmp, size);

  if UNLIKELY(n >= size) {
    mem_freetv(ma, result, size);
    return NULL;
  }

  return result;
}


char* nullable path_join(memalloc_t ma, const char* path1, const char* path2) {
  return path_joinslice(ma, slice_cstr(path1), slice_cstr(path2));
}

