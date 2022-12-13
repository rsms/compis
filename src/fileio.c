// SPDX-License-Identifier: Apache-2.0
#include "c0lib.h"
#include "llvm/llvm.h"
#include <fcntl.h> // open
#include <unistd.h> // close
#include <sys/stat.h>
#include <sys/mman.h> // mmap


err_t mmap_file(const char* filename, mem_t* data_out) {
  int fd = open(filename, O_RDONLY);
  if (fd < 0)
    return err_errno();

  struct stat st;
  if (fstat(fd, &st) != 0) {
    err_t err = err_errno();
    close(fd);
    return err;
  }

  void* p = mmap(0, (usize)st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
  close(fd);
  if (p == MAP_FAILED)
    return ErrNoMem;

  data_out->p = p;
  data_out->size = (usize)st.st_size;
  return 0;
}


err_t mmap_unmap(mem_t m) {
  if (munmap(m.p, m.size) == 0)
    return 0;
  return err_errno();
}


err_t writefile(const char* filename, u32 mode, slice_t data) {
  if (data.len > (usize)ISIZE_MAX)
    return ErrOverflow;
  int fd = open(filename, O_WRONLY|O_CREAT|O_TRUNC, 0777);
  if (fd < 0)
    return err_errno();
  err_t err = 0;
  while (data.len) {
    isize n = write(fd, data.p, data.len);
    if (n < (isize)data.len) {
      err = n < 0 ? err_errno() : ErrCanceled;
      break;
    }
    data.p += n;
    data.len -= (usize)n;
  }
  close(fd);
  return err;
}


err_t fs_mkdirs(const char* path, usize pathlen, int perms) {
  return err_errnox(LLVMCreateDirectories(path, pathlen, perms));
}
