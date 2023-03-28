// SPDX-License-Identifier: Apache-2.0
#include "colib.h"

#include <err.h>
#include <errno.h>
#include <fcntl.h>


// currently we rely on fcntl F_SETLK
#if !defined(F_SETLK) || !defined(F_SETLKW)
  #error lockfile not implemented for target platform
#endif
#define USE_SETLK


err_t fs_lock(int fd) {
  struct flock fl = { .l_type = F_WRLCK, .l_whence = SEEK_SET };
  if (fcntl(fd, F_SETLKW, &fl) == 0)
    return 0;
  return err_errno();
}


err_t fs_trylock(int fd, long* nullable lockee_pid) {
  struct flock fl = { .l_type = F_WRLCK, .l_whence = SEEK_SET };
  if (fcntl(fd, F_SETLK, &fl) == 0) {
    // lock was acquired
    return 0;
  }

  if (errno != EAGAIN && errno != EACCES) {
    // an error occurred
    return err_errno();
  }

  // failed to acquire lock (it's held by someone else.)
  // if lockee_pid pointer is provided, request lockee pid
  if (lockee_pid) {
    if (fcntl(fd, F_GETLK, &fl) == 0) {
      *lockee_pid = fl.l_pid;
    } else {
      // failure to get lockee pid is a soft error, one that we only warn about
      // note: don't warn about ENOENT (lost race)
      *lockee_pid = -1;
      if (errno != ENOENT)
        elog("warning: fcntl(F_GETLK) failed: %s", err_str(err_errno()));
    }
  }
  return ErrExists;
}


err_t fs_unlock(int fd) {
  struct flock fl = { .l_type = F_UNLCK, .l_whence = SEEK_SET };
  if (fcntl(fd, F_SETLKW, &fl) == 0)
    return 0;
  // failed to unlock
  err_t err = err_errno();
  elog("%s/fcntl(F_SETLKW, F_UNLCK): %s", __FUNCTION__, err_str(err));
  return err;
}
