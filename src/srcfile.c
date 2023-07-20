// SPDX-License-Identifier: Apache-2.0
#include "colib.h"
#include "compiler.h"
#include "path.h"

#include <sys/stat.h>


static int srcfile_name_cmp(const srcfile_t* a, const srcfile_t* b, void* ctx) {
  return strcmp(a->name.p, b->name.p);
}

static int srcfile_addr_cmp(const srcfile_t* a, const srcfile_t* b, void* ctx) {
  return a == b ? 0 : a < b ? -1 : 1;
}


isize srcfilearray_indexof(const srcfilearray_t* srcfiles, const srcfile_t* f) {
  u32 index = 0;
  if (!array_sortedset_lookup(
    srcfile_t, (array_t*)srcfiles, f, &index,
    (array_sorted_cmp_t)srcfile_addr_cmp, NULL))
  {
    return -1;
  }
  assert((usize)index <= (usize)ISIZE_MAX);
  if ((usize)index > (usize)ISIZE_MAX)
    return -1;
  return (isize)index;
}


srcfile_t* srcfilearray_add(
  srcfilearray_t* srcfiles, const char* name, usize namelen, bool* addedp)
{
  // add to sorted set, or retrieve existing with same name
  srcfile_t tmp = { .name = str_makelen(name, namelen) };
  srcfile_t* f = array_sortedset_assign(
    srcfile_t, (array_t*)srcfiles, memalloc_ctx(),
    &tmp, (array_sorted_cmp_t)srcfile_name_cmp, NULL);

  if (f == NULL || f->name.p != NULL) {
    // memory-allocation failure or file already added
    str_free(tmp.name);
    if (addedp)
      *addedp = false;
    return f;
  }

  // note: array_sortedset_assign returns new entries zero initialized

  f->pkg = NULL;
  f->name = tmp.name;
  f->type = filetype_guess(f->name.p);
  if (addedp)
    *addedp = true;
  return f;
}


void srcfilearray_dispose(srcfilearray_t* srcfiles) {
  for (u32 i = 0; i < srcfiles->len; i++)
    srcfile_dispose(&srcfiles->v[i]);
  array_dispose(srcfile_t, (array_t*)srcfiles, memalloc_ctx());
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
