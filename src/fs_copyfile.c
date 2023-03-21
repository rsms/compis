// SPDX-License-Identifier: Apache-2.0
#include "colib.h"
#include "path.h"
#include "dirwalk.h"

#include <dirent.h>
#include <err.h>
#include <errno.h>
#include <fcntl.h> // open
#include <string.h>
#include <sys/mman.h> // mmap
#include <sys/stat.h>
#include <unistd.h> // close
#ifdef __APPLE__
  #include <sys/clonefile.h>
#endif


static isize copy_fd_fd(int src_fd, int dst_fd, char* buf, usize bufsize) {
  isize wresid, wcount = 0;
  char *bufp;
  isize rcount = read(src_fd, buf, bufsize);
  if (rcount <= 0)
    return rcount;
  for (bufp = buf, wresid = rcount; ; bufp += wcount, wresid -= wcount) {
    wcount = write(dst_fd, bufp, wresid);
    if (wcount <= 0)
      break;
    if (wcount >= (isize)wresid)
      break;
  }
  return wcount < 0 ? wcount : rcount;
}


static err_t copy_file(const char* src, const char* dst, int flags) {
  int src_fd = -1, dst_fd = -1;
  char* dstdir = NULL;
  int nretries = 0;
  int errno2;

retry:
  errno = 0;

  #if defined(__APPLE__)
    if (clonefile(src, dst, /*flags*/0) == 0)
      goto end;
  #endif

  // TODO: look into using ioctl_ficlone or copy_file_range on linux

  if (errno != EEXIST) {
    // fall back to byte copying
    if ((src_fd = open(src, O_RDONLY, 0)) == -1)
      goto end;
    struct stat src_st;
    if (fstat(src_fd, &src_st) != 0)
      goto end;
    mode_t dst_mode = src_st.st_mode & ~(S_ISUID | S_ISGID);
    if ((dst_fd = open(dst, O_WRONLY|O_TRUNC|O_CREAT, dst_mode)) == -1)
      goto end;

    char buf[4096];
    isize rcount;
    for (;;) {
      rcount = copy_fd_fd(src_fd, dst_fd, buf, sizeof(buf));
      if (rcount == 0)
        goto end;
      if (rcount < 0)
        break;
    }
    close(src_fd); src_fd = -1;
    close(dst_fd); dst_fd = -1;
  }

  if (++nretries == 2) {
    // end
  } else if (errno == ENOENT && dstdir == NULL) {
    dstdir = path_dir_alloca(dst);
    fs_mkdirs(dstdir, 0755, flags);
    goto retry;
  } else if (errno == EEXIST) {
    unlink(dst);
    goto retry;
  }

end:
  errno2 = errno;
  if (src_fd != -1) close(src_fd);
  if (dst_fd != -1) close(dst_fd);
  return err_errnox(errno2);
}


static err_t copy_symlink(const char* src, const char* dst, mode_t mode, int flags) {
  char target[PATH_MAX];
  char* dstdir = NULL;
  int nretries = 0;

  isize len = readlink(src, target, sizeof(target));
  if (len < 0)
    return err_errno();
  if (len >= (isize)sizeof(target))
    return ErrOverflow;
  target[len] = '\0';

  mode &= (S_IRWXU | S_IRWXG | S_IRWXO);

  if (flags & FS_VERBOSE)
    vlog("create symlink %s -> %s", relpath(dst), relpath(target));

retry:
  if (symlink(target, dst) == 0) {
    int chmod_flags = AT_SYMLINK_NOFOLLOW;
    if (fchmodat(AT_FDCWD, dst, mode, chmod_flags) != 0 && errno != ENOTSUP) {
      err_t err = err_errno();
      elog("failed to set mode on symlink %s: %s", relpath(dst), strerror(errno));
      return err;
    }
    return 0;
  }

  if (++nretries == 2) {
    // end
  } else if (errno == ENOENT && dstdir == NULL) {
    dstdir = path_dir_alloca(dst);
    fs_mkdirs(dstdir, 0755, flags); // TODO: handle error
    goto retry;
  } else if (errno == EEXIST) {
    unlink(dst);
    goto retry;
  } else {
    elog("failed to create symlink %s: %s", dst, strerror(errno));
  }

  return err_errno();
}


static err_t _mkdirs(const char* path, mode_t mode, int flags) {
  mode &= (S_IRWXU | S_IRWXG | S_IRWXO);
  err_t err = fs_mkdirs(path, mode, flags);
  if (err)
    elog("failed to create directory '%s': %s", path, err_str(err));
  return err;
}


static err_t copy_dir(const char* src, const char* dst, mode_t mode, int flags) {
  err_t err;
  memalloc_t ma = memalloc_ctx();

  dirwalk_t* dw = dirwalk_open(ma, src, 0);
  if (!dw)
    return ErrNoMem;

  const char* srcdir = dirwalk_parent_path(dw);
  usize srclen = strlen(srcdir);

  char* dstpath = mem_alloc(ma, PATH_MAX).p;
  if (!dstpath) {
    err = ErrNoMem;
    goto end;
  }
  usize dstlen = strlen(dst);
  memcpy(dstpath, dst, dstlen);

  // create destination directory (mask mode to get only the permission bits)
  if (( err = _mkdirs(dst, mode, flags) ))
    goto end1;

  while (!err && (err = dirwalk_next(dw)) > 0) {
    const char* relpath = dw->path + srclen;
    usize relpathlen = strlen(relpath);
    memcpy(dstpath + dstlen, relpath, relpathlen + 1);

    switch (dw->type) {
      case S_IFDIR:
        dirwalk_descend(dw);
        mode = dirwalk_lstat(dw)->st_mode;
        err = _mkdirs(dstpath, mode, flags);
        break;
      case S_IFREG:
        err = copy_file(dw->path, dstpath, flags);
        break;
      case S_IFLNK:
        mode = dirwalk_lstat(dw)->st_mode;
        err = copy_symlink(dw->path, dstpath, mode, /*verbose*/false);
        break;
      default:
        err = ErrInvalid;
        vlog("cannot copy %s: unsupported type", dw->path);
        break;
    }
  }

end1:
  mem_freex(ma, MEM(dstpath, PATH_MAX));
end:
  dirwalk_close(dw);
  return err;
}


err_t fs_copyfile(const char* src, const char* dst, int flags) {
  struct stat st;
  if (lstat(src, &st) != 0)
    return err_errno();

  switch (st.st_mode & S_IFMT) {
    case S_IFDIR:
      if (flags & FS_VERBOSE)
        vlog("copy directory %s -> %s", relpath(src), relpath(dst));
      return copy_dir(src, dst, st.st_mode, flags);
    case S_IFREG:
      if (flags & FS_VERBOSE)
        vlog("copy file %s -> %s", relpath(src), relpath(dst));
      return copy_file(src, dst, flags);
    case S_IFLNK:
      return copy_symlink(src, dst, st.st_mode, flags);
    default:
      vlog("cannot copy %s: unsupported type", src);
      return ErrInvalid;
  }
}
