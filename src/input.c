// SPDX-License-Identifier: Apache-2.0
#include "c0lib.h"
#include "compiler.h"


input_t* nullable input_create(memalloc_t ma, const char* filename) {
  usize filename_len = strlen(filename);
  usize size = filename_len + 1 + sizeof(input_t);
  mem_t m = mem_alloc(ma, size);
  if (m.p == NULL)
    return NULL;

  input_t* input = m.p;
  memset(input, 0, sizeof(*input));
  memcpy(input->name, filename, filename_len);
  input->name[filename_len] = 0;

  input->type = filetype_guess(input->name);

  return input;
}


void input_free(input_t* input, memalloc_t ma) {
  if (input->data.p != NULL)
    input_close(input);
  mem_t m = MEM(input, sizeof(input_t) + strlen(input->name) + 1);
  mem_free(ma, &m);
  #if C0_SAFE
    memset(input, 0, m.size);
  #endif
}


err_t input_open(input_t* input) {
  assert(input->data.p == NULL);
  assert(input->data.size == 0);
  err_t err = mmap_file(input->name, &input->data);
  input->ismmap = err == 0;
  return err;
}


void input_close(input_t* input) {
  if (input->ismmap) {
    mmap_unmap(input->data);
    input->ismmap = false;
  }
  memset(&input->data, 0, sizeof(input->data));
}


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
