// growable argv-compatible string array with efficient memory storage
// SPDX-License-Identifier: Apache-2.0
ASSUME_NONNULL_BEGIN

typedef struct {
  int   fd;
  void* _internal;
} lockfile_t;

err_t lockfile_lock(lockfile_t* lf, const char* filename); // blocks until locked

// lockfile_trylock tries to acquire the lock without blocking.
// If the lock cannot be acquired, ErrExists is returned.
// If lockee_pid is prodivided, it is set to the PID of the lockee in case of ErrExists.
// CAVEAT: Caller providing lockee_pid should always check it for <0 (error) even when
// this function succeeds. This because that in rare occasions the lockee may take
// a long time to write its pid; this function will only wait for a successful write for
// a short amount of time before giving up and setting *lockee_pid = -1.
err_t lockfile_trylock(lockfile_t* lf, const char* filename, long* nullable lockee_pid);

err_t lockfile_unlock(lockfile_t* lf);

ASSUME_NONNULL_END
