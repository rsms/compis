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
