// SPDX-License-Identifier: Apache-2.0
#include "colib.h"
#include "llvm/llvm.h"
#include "path.h"

#include <fcntl.h> // open
#include <err.h>
#include <errno.h>
#include <dirent.h>
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


err_t fs_mkdirs(const char* path, int perms) {
  dlog("mkdirs %s", relpath(path));
  return err_errnox(LLVMCreateDirectories(path, strlen(path), perms));
}


err_t fs_remove(const char* path) {
  struct stat st;

  if (lstat(path, &st) != 0)
    return err_errno();

  if (!S_ISDIR(st.st_mode)) {
    if (unlink(path) != 0) {
      warn("unlink(%s)", path);
      return err_errno();
    }
    return 0;
  }

  DIR* dp = opendir(path);
  if (dp == NULL) {
    warn("opendir(%s)", path);
    return err_errno();
  }
  struct dirent* d;

  char path2[PATH_MAX];
  usize pathlen = MIN(strlen(path), PATH_MAX - 2);
  memcpy(path2, path, pathlen);
  path2[pathlen++] = '/';

  while ((d = readdir(dp)) != NULL) {
    if (*d->d_name == '.' && (!d->d_name[1] || (d->d_name[1] == '.' && !d->d_name[2])))
      continue;
    usize namelen = MIN(strlen(d->d_name), PATH_MAX - pathlen);
    memcpy(&path2[pathlen], d->d_name, namelen);
    path2[pathlen + namelen] = 0;
    err_t err = fs_remove(path2);
    if (err)
      return err;
  }
  closedir(dp);
  if (rmdir(path) != 0) {
    warn("rmdir(%s)", path);
    return err_errno();
  }
  return 0;
}


bool fs_isfile(const char* path) {
  struct stat st;
  if (stat(path, &st) != 0)
    return false;
  return S_ISREG(st.st_mode);
}


bool fs_isdir(const char* path) {
  struct stat st;
  if (stat(path, &st) != 0)
    return false;
  return S_ISDIR(st.st_mode);
}
