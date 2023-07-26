// SPDX-License-Identifier: Apache-2.0
#include "colib.h"
#include "compiler.h"
#include "path.h"

#include <sys/stat.h>


static int srcfile_name_cmp(const srcfile_t** a, const srcfile_t** b, void* ctx) {
  return strcmp((*a)->name.p, (*b)->name.p);
}


srcfile_t* nullable srcfilearray_add(
  ptrarray_t* srcfiles, const char* name, usize namelen, bool* addedp)
{
  // add to sorted set, or retrieve existing with same name
  srcfile_t tmp = { .name = str_makelen(name, namelen) };
  srcfile_t* tmp1 = &tmp;
  srcfile_t** fp = array_sortedset_assign(
    srcfile_t*, (array_t*)srcfiles, memalloc_ctx(),
    &tmp1, (array_sorted_cmp_t)srcfile_name_cmp, NULL);

  if (fp == NULL || *fp != NULL) {
    // memory-allocation failure or file already added
    str_free(tmp.name);
    if (addedp)
      *addedp = false;
    return *fp;
  }

  srcfile_t* f = mem_alloct(memalloc_ctx(), srcfile_t);
  if UNLIKELY(!f)
    return NULL;
  *fp = f;

  f->pkg = NULL;
  f->name = tmp.name;
  f->type = filetype_guess(f->name.p);
  if (addedp)
    *addedp = true;
  return f;
}


void srcfilearray_dispose(ptrarray_t* srcfiles) {
  memalloc_t ma = memalloc_ctx();
  for (u32 i = 0; i < srcfiles->len; i++) {
    srcfile_t* f = srcfiles->v[i];
    srcfile_dispose(f);
    mem_freet(ma, f);
  }
  ptrarray_dispose(srcfiles, ma);
}


void srcfile_dispose(srcfile_t* sf) {
  str_free(sf->name);
}


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

  struct stat st;
  err_t err = mmap_file_ro(path.p, &sf->data, &st);
  sf->ismmap = err == 0;
  sf->size = st.st_size;

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
