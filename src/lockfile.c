// SPDX-License-Identifier: Apache-2.0
#include "colib.h"
#include "lockfile.h"
#include "path.h"

#include <stdlib.h>
#include <unistd.h>
#include <err.h>
#include <limits.h>
#include <fcntl.h>

#include <errno.h>
#include <string.h>


// #undef O_EXLOCK // XXX


err_t lockfile_lock(lockfile_t* lf, const char* filename) {
  err_t err;

  #if defined(O_EXLOCK)
    lf->fd = open(filename, O_WRONLY | O_CREAT | O_TRUNC | O_EXLOCK, 0666);
    if (lf->fd < 0)
      return err_errno();
  #elif defined(F_SETLKW)
    lf->fd = open(filename, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (lf->fd < 0)
      return err_errno();
    struct flock fl = { .l_type = F_WRLCK, .l_whence = SEEK_SET };
    if (fcntl(lf->fd, F_SETLKW, &fl) < 0)
      goto error;
  #else
    #error lockfile_lock not implemented for target platform
  #endif

  // write lockee's pid to lockfile
  char pidbuf[16];
  usize len = sfmtu64(pidbuf, (u64)getpid(), 10);
  if (write(lf->fd, pidbuf, len) < (isize)len)
    goto error;

  #if !defined(F_GETPATH)
    lf->_internal = strdup(filename);
    if (!lf->_internal)
      goto error;
  #endif

  return 0;
error:
  err = err_errno();
  close(lf->fd);
  return err;
}


err_t lockfile_trylock(lockfile_t* lf, const char* filename, long* nullable lockee_pid) {
  err_t err = 0;
  char pidbuf[16];
  isize len;
  char* dstdir = NULL;
  int openflags;

  #if defined(O_EXLOCK) && defined(O_NONBLOCK)
    openflags = O_CREAT | O_EXLOCK | O_NONBLOCK
              | (lockee_pid ? O_RDWR : (O_WRONLY | O_TRUNC));
  #else
    openflags = O_CREAT
              | (lockee_pid ? O_RDWR : (O_WRONLY | O_TRUNC));
  #endif

open:
  lf->fd = open(filename, openflags, 0666);
  if (lf->fd < 0) {
    if (errno == EAGAIN)
      goto lockfail;
    if (errno == ENOENT && !dstdir) {
      dstdir = path_dir_alloca(filename);
      if ((err = fs_mkdirs(dstdir, 0755)) == 0)
        goto open;
    }
    warn("open(%s)", filename);
    return err ? err : err_errno();
  }

  #if !defined(O_EXLOCK) && !defined(O_NONBLOCK)
    #if defined(F_SETLK)
      struct flock fl = { .l_type = F_WRLCK, .l_whence = SEEK_SET };
      if (fcntl(lf->fd, F_SETLK, &fl) < 0) {
        if (errno == EAGAIN)
          goto lockfail;
        warn("fcntl(F_SETLK)");
        goto error;
      }
    #else
      #error lockfile_trylock not implemented for target platform
    #endif
  #endif

  // write our pid to lockfile
  if (lockee_pid)
    ftruncate(lf->fd, 0);
  len = (isize)sfmtu64(pidbuf, (u64)getpid(), 10);
  if (write(lf->fd, pidbuf, (usize)len) < len)
    goto error;

  #if !defined(F_GETPATH)
    lf->_internal = strdup(filename);
    if (!lf->_internal)
      goto error;
  #endif

  return 0;

error:
  if (err == 0)
    err = err_errno();
  close(lf->fd);
  return err;

lockfail:
  dlog("lockfail");
  if (lockee_pid) {
    #if defined(O_EXLOCK) && defined(O_NONBLOCK)
      lf->fd = open(filename, O_RDONLY, 0);
    #endif
    if (lf->fd < 0) {
      *lockee_pid = -1;
    } else {
      if ((len = read(lf->fd, pidbuf, sizeof(pidbuf))) < 0) {
        warn("read %s", filename);
        return err_errno();
      }
      pidbuf[MIN((usize)len, sizeof(pidbuf)-1)] = 0;
      char* end;
      unsigned long n = strtoul(pidbuf, &end, 10);
      if (n == ULONG_MAX || n > LONG_MAX || *end || (n == 0 && errno)) {
        warnx("bad pid in lockfile %s", filename);
        *lockee_pid = -1;
      } else {
        *lockee_pid = (long)n;
      }
    }
  }
  return ErrExists;
}


err_t lockfile_unlock(lockfile_t* lf) {
  int r;

  #if defined(F_GETPATH)
    char filename[PATH_MAX];
    if (( r = fcntl(lf->fd, F_GETPATH, filename) ) == 0)
      r = unlink(filename);
  #else
    r = unlink(lf->_internal);
    free(lf->_internal);
  #endif

  #if !defined(O_EXLOCK)
    struct flock fl = { .l_type = F_UNLCK, .l_whence = SEEK_SET };
    r = fcntl(lf->fd, F_SETLKW, &fl) == 0 ? r : -1;
  #endif

  r = close(lf->fd) == 0 ? r : -1;

  #if DEBUG
    memset(lf, 0, sizeof(*lf));
  #endif

  return r;
}
