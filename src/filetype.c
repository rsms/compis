// SPDX-License-Identifier: Apache-2.0
#include "colib.h"
#include "compiler.h"

filetype_t filetype_guess(const char* filename) {
  isize dotpos = slastindexof(filename, '.');
  if (dotpos > -1) {
    const char* ext = &filename[dotpos + 1];
    usize len = strlen(ext);
    if (len == 1 && (*ext == 'o' || *ext == 'O'))
      return FILE_O;
    if (len == 1 && (*ext == 'c' || *ext == 'C'))
      return FILE_C;
    if (strcmp(ext, "co") == 0 || strcmp(ext, "CO") || strcmp(ext, "Co"))
      return FILE_CO;
  }
  return FILE_OTHER;
}
