// SPDX-License-Identifier: Apache-2.0
#include "colib.h"
#include "compiler.h"
#include "path.h"

#include <sys/stat.h>


// str_t srcfile_path(srcfile_t* sf) {
//   if (sf->pkg->dir.len)
//     return path_join(sf->pkg->dir.p, sf->name.p);
//   return str_makelen(sf->name.p, sf->name.len);
// }


err_t srcfile_open(srcfile_t* sf) {
  if (sf->data)
    return 0;

  str_t path;
  if (sf->pkg->dir.len) {
    path = path_join(sf->pkg->dir.p, sf->name.p);
  } else {
    path = sf->name;
  }

  err_t err = mmap_file_ro(path.p, sf->size, &sf->data);
  sf->ismmap = err == 0;

  if (sf->pkg->dir.len)
    str_free(path);

  return err;
}


void srcfile_close(srcfile_t* sf) {
  if (sf->ismmap) {
    mmap_unmap(sf->data, sf->size);
    sf->ismmap = false;
  } else if (sf->data != NULL) {
    dlog("TODO: free srcfile.data");
  }
  sf->data = NULL;
}
