// SPDX-License-Identifier: Apache-2.0
#include "colib.h"
#include "lockfile.h"
#include "path.h"

#include <err.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h> // strdup
#include <unistd.h>


// locking mechanism
#if defined(F_SETLK) && defined(F_SETLKW)
  #define USE_SETLK
#elif defined(O_EXLOCK) && defined(O_NONBLOCK)
  #warning lockfile using untested O_EXLOCK mechanism
#else
  #error lockfile not implemented for target platform
#endif

// USE_FCNTL_GETPATH: use fcntl(F_GETPATH) to get filename
#ifdef F_GETPATH
  #define USE_FCNTL_GETPATH
#endif


err_t lockfile_lock(lockfile_t* lf, const char* filename) {
  err_t err;

  int openflags;
  #ifdef USE_SETLK
    openflags = O_WRONLY | O_CREAT | O_TRUNC;
  #else
    openflags = O_WRONLY | O_CREAT | O_TRUNC | O_EXLOCK;
  #endif

  lf->fd = open(filename, openflags, 0666);
  if (lf->fd < 0)
    return err_errno();

  #ifdef USE_SETLK
    struct flock fl = { .l_type = F_WRLCK, .l_whence = SEEK_SET };
    if (fcntl(lf->fd, F_SETLKW, &fl) < 0)
      goto error;
  #else
    // write lockee's pid to lockfile
    char pidbuf[16];
    usize len = sfmtu64(pidbuf, (u64)getpid(), 10);
    pidbuf[len++] = '\n';
    if (write(lf->fd, pidbuf, len) < (isize)len)
      goto error;
  #endif

  #if !defined(USE_FCNTL_GETPATH)
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

  int openflags;
  #ifdef USE_SETLK
    openflags = O_CREAT;
  #else
    isize len;
    char pidbuf[16];
    openflags = O_CREAT | O_EXLOCK | O_NONBLOCK;
  #endif
  openflags |= lockee_pid ? O_RDWR : (O_WRONLY | O_TRUNC);

  lf->fd = open(filename, openflags, 0666);
  if (lf->fd < 0) {
    if (errno == EAGAIN)
      goto lockfail;
    return err_errno();
  }

  #ifdef USE_SETLK
    struct flock fl = { .l_type = F_WRLCK, .l_whence = SEEK_SET };
    if (fcntl(lf->fd, F_SETLK, &fl) < 0) {
      if (errno == EAGAIN)
        goto lockfail;
      warn("fcntl(F_SETLK)");
      goto error;
    }
  #else
    // write our pid to lockfile
    if (lockee_pid) // opened with O_RDWR
      ftruncate(lf->fd, 0);
    len = (isize)sfmtu64(pidbuf, (u64)getpid(), 10);
    pidbuf[len++] = '\n';
    if (write(lf->fd, pidbuf, (usize)len) < len)
      goto error;
  #endif

  #if !defined(USE_FCNTL_GETPATH)
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
  if (!lockee_pid)
    return ErrExists;

  #ifdef USE_SETLK
    assert(lf->fd > -1);
    if (fcntl(lf->fd, F_GETLK, &fl) != 0 && errno != ENOENT) {
      err = err_errno();
      elog("%s: fcntl(F_GETLK) failed: %s", __FUNCTION__, err_str(err));
      return err;
    }
    *lockee_pid = fl.l_pid;
  #else
    // reopen
    lf->fd = open(filename, O_RDONLY, 0);
    if (lf->fd < 0) {
      *lockee_pid = -1;
      return ErrExists;
    }
    // try reading lockee pid, waiting up to ~100ms
    for (u32 nattempts = 10;;) {
      if ((len = read(lf->fd, pidbuf, sizeof(pidbuf))) < 0) {
        warn("read %s", filename);
        return err_errno();
      }
      if (len > 1 && pidbuf[len - 1] == '\n')
        break;
      if (--nattempts == 0) {
        // give up
        *lockee_pid = -1;
        goto end;
      }
      // sleep 10ms then try again
      microsleep(1000*10);
    }
    char* end;
    pidbuf[len - 1] = 0;
    unsigned long n = strtoul(pidbuf, &end, 10);
    if (n == ULONG_MAX || n > LONG_MAX || *end || (n == 0 && errno)) {
      elog("%s: bad pid in lockfile %s: \"%s\"", __FUNCTION__, filename, pidbuf);
      *lockee_pid = -1;
    } else {
      *lockee_pid = (long)n;
    }
end:
  #endif

  return ErrExists;
}


err_t lockfile_unlock(lockfile_t* lf) {
  int r;

  #ifdef USE_FCNTL_GETPATH
    char filename[PATH_MAX];
    if (( r = fcntl(lf->fd, F_GETPATH, filename) ) == 0)
      r = unlink(filename);
  #else
    r = unlink(lf->_internal);
    free(lf->_internal);
  #endif

  #if !defined(O_EXLOCK) && defined(F_SETLKW)
    struct flock fl = { .l_type = F_UNLCK, .l_whence = SEEK_SET };
    r = fcntl(lf->fd, F_SETLKW, &fl) == 0 ? r : -1;
  #endif

  r = close(lf->fd) == 0 ? r : -1;

  #if DEBUG
    memset(lf, 0, sizeof(*lf));
  #endif

  return r;
}
