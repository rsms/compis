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
err_t lockfile_trylock(lockfile_t* lf, const char* filename, long* nullable lockee_pid);

err_t lockfile_unlock(lockfile_t* lf);

ASSUME_NONNULL_END
