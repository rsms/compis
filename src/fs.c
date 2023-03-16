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
#ifdef __APPLE__
  #include <sys/clonefile.h>
#endif


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
  int fd = open(filename, O_WRONLY|O_CREAT|O_TRUNC, mode);
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


static err_t _fs_mkdirs(const char* path, int perms, bool verbose) {
  // copy path into mutable storage
  usize len = strlen(path);
  if (len == 0) return ErrInvalid;
  char* buf = alloca(len + 1);
  memcpy(buf, path, len + 1);

  char* s = buf + len;
  char* end;
  struct stat st;

  // trim away trailing separators, e.g. "/a/b//" => "/a/b"
  while (*--s == PATH_SEPARATOR) {
    if (s == buf)
      return 0; // path is "/"
  }
  if (s == buf && *buf == '.')
    return 0; // path is "."
  s[1] = 0;
  end = s;

  // stat from leaf to root, e.g "/a/b/c", "/a/b", "/a"
  for (;;) {
    if (stat(buf, &st) == 0) {
      if (!S_ISDIR(st.st_mode))
        return ErrNotDir;
      break;
    }
    if (errno != ENOENT)
      goto err;

    // skip past the last component
    while (--s > buf) {
      if (*s == PATH_SEPARATOR) {
        // skip past any trailing separators
        while (*--s == PATH_SEPARATOR) {};
        break;
      }
    }
    if (s == buf)
      break;
    // replace path separator with null terminator
    s[1] = 0;
  }

  if (s < end && verbose)
    log("creating directory '%s'", relpath(path));

  // mkdir starting with the first non-existant dir, e.g "/a", "/a/b", "/a/b/c"
  while (s < end) {
    if (mkdir(buf, perms) < 0 && errno != EEXIST) {
      dlog("mkdir %s: %s", buf, err_str(err_errno()));
      goto err;
    }
    while (++s < end && *s) {}
    *s = PATH_SEPARATOR;
  }

  return 0;
err:
  return err_errno();
}


err_t fs_mkdirs(const char* path, int perms) {
  return _fs_mkdirs(path, perms, /*verbose*/false);
}


err_t fs_mkdirs_verbose(const char* path, int perms) {
  return _fs_mkdirs(path, perms, coverbose);
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


static err_t fs_copyfile_reg(const char* srcpath, const char* dstpath) {
  int src_fd = -1, dst_fd = -1;
  char* dstdir = NULL;
  int nretries = 0;

retry:
  errno = 0;

  #if defined(__APPLE__)
    if (clonefile(srcpath, dstpath, /*flags*/0) == 0)
      goto end;
  #endif

  // TODO: look into using ioctl_ficlone or copy_file_range on linux

  if (errno != EEXIST) {
    // fall back to byte copying
    if ((src_fd = open(srcpath, O_RDONLY, 0)) == -1)
      goto end;
    struct stat src_st;
    if (fstat(src_fd, &src_st) != 0)
      goto end;
    mode_t dst_mode = src_st.st_mode & ~(S_ISUID | S_ISGID);
    if ((dst_fd = open(dstpath, O_WRONLY|O_TRUNC|O_CREAT, dst_mode)) == -1)
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
    dstdir = path_dir_alloca(dstpath);
    fs_mkdirs(dstdir, 0755);
    goto retry;
  } else if (errno == EEXIST) {
    unlink(dstpath);
    goto retry;
  }

end:
  if (src_fd != -1) close(src_fd);
  if (dst_fd != -1) close(dst_fd);
  return err_errno();
}


static err_t fs_copyfile_link(const char* srcpath, const char* dstpath) {
  char target[PATH_MAX];
  char* dstdir = NULL;
  int nretries = 0;

  isize len = readlink(srcpath, target, sizeof(target));
  if (len < 0)
    return err_errno();
  if (len >= (isize)sizeof(target))
    return ErrOverflow;
  target[len] = '\0';

retry:
  if (symlink(target, dstpath) == 0)
    return 0;

  if (++nretries == 2) {
    // end
  } else if (errno == ENOENT && dstdir == NULL) {
    dstdir = path_dir_alloca(dstpath);
    fs_mkdirs(dstdir, 0755);
    goto retry;
  } else if (errno == EEXIST) {
    unlink(dstpath);
    goto retry;
  } else {
    warn("symlink: %s", dstpath);
  }

  return err_errno();
}


err_t fs_copyfile(const char* srcpath, const char* dstpath, int flags) {
  struct stat st;
  if (lstat(srcpath, &st) != 0)
    return err_errno();
  if (S_ISLNK(st.st_mode))
    return fs_copyfile_link(srcpath, dstpath);
  if (S_ISREG(st.st_mode))
    return fs_copyfile_reg(srcpath, dstpath);
  return ErrInvalid;
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
