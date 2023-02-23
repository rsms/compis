// SPDX-License-Identifier: Apache-2.0
#include "colib.h"
#include "subproc.h"

// enable posix_spawn_file_actions_addchdir_np
#if defined(__APPLE__) || defined(__linux__)
  #define _DARWIN_C_SOURCE
  #define _BSD_SOURCE
  #define HAS_SPAWN_ADDCHDIR
#endif

#include <sys/wait.h> // waitpid
#include <errno.h> // ECHILD
#include <unistd.h> // fork, pgrp
#include <err.h>
#include <signal.h>
#include <spawn.h>


#define trace(fmt, va...) _trace(opt_trace_subproc, 3, "subproc", fmt, ##va)


#ifdef __APPLE__
  // waitpid(-pgrp) isn't reliable on macOS/darwin.
  // Thank you to Julio Merino for these functions.
  // https://jmmv.dev/2019/11/wait-for-process-group-darwin.html
  #include <sys/event.h>
  #include <sys/sysctl.h>
  #include <sys/types.h>

  #include <assert.h>
  #include <errno.h>
  #include <stddef.h>
  #include <stdlib.h>
  #include <unistd.h>

  static int darwin_pgrp_wait(pid_t pgid) {
    int name[] = {CTL_KERN, KERN_PROC, KERN_PROC_PGRP, pgid};

    for (;;) {
      // Query the list of processes in the group by using sysctl(3).
      // This is "hard" because we don't know how big that list is, so we
      // have to first query the size of the output data and then account for
      // the fact that the size might change by the time we actually issue
      // the query.
      struct kinfo_proc *procs = NULL;
      usize nprocs = 0;
      do {
        usize len;
        if (sysctl(name, 4, 0, &len, NULL, 0) == -1)
          return -1;
        procs = (struct kinfo_proc *)malloc(len);
        if (sysctl(name, 4, procs, &len, NULL, 0) == -1) {
          assert(errno == ENOMEM);
          free(procs);
          procs = NULL;
        } else {
          nprocs = len / sizeof(struct kinfo_proc);
        }
      } while (procs == NULL);
      assert(nprocs >= 1);  // Must have found the group leader at least.

      if (nprocs == 1) {
        // Found only one process, which must be the leader because we have
        // purposely expect it as a zombie.
        assert(procs->kp_proc.p_pid == pgid);
        free(procs);
        return 0;
      }

      // More than one process left in the process group.
      // Pause a little bit before retrying to avoid burning CPU.
      struct timespec ts;
      ts.tv_sec = 0;
      ts.tv_nsec = 1000000;
      if (nanosleep(&ts, NULL) == -1)
        return -1;
    }
  }

  static int darwin_wait_pid(pid_t pid) {
    int kq;
    if ((kq = kqueue()) == -1)
      return -1;

    struct kevent kc;
    EV_SET(&kc, pid, EVFILT_PROC, EV_ADD | EV_ENABLE, NOTE_EXIT, 0, 0);

    int nev;
    struct kevent ev;
    if ((nev = kevent(kq, &kc, 1, &ev, 1, NULL)) == -1)
      return -1;

    assert(nev == 1);
    assert((pid_t)ev.ident == pid);
    assert(ev.fflags & NOTE_EXIT);

    return close(kq);
  }
#endif // __APPLE__


void subproc_open(subproc_t* p, pid_t pid) {
  assert(p->pid == 0);
  memset(p, 0, sizeof(*p));
  p->pid = pid;
}


void subproc_close(subproc_t* p) {
  assert(p->pid != 0);
  p->pid = 0;
}


err_t subproc_await(subproc_t* p) {
  if (p->pid == 0)
    return ErrCanceled;

  if (p->err) {
    subproc_close(p);
    return p->err;
  }

  #if defined(__APPLE__)
    // wait for process-group leader
    darwin_wait_pid(p->pid);

    // wait for other process in the group
    darwin_pgrp_wait(p->pid);

    // get leader exit status
    int status;
    if (waitpid(p->pid, &status, 0) == -1 || !WIFEXITED(status)) {
      trace("proc [%d] died or experienced an error", p->pid);
      p->err = ErrCanceled;
    } else {
      status = WEXITSTATUS(status);
      if (status != 0)
        p->err = status < 0 ? ErrInvalid : (err_t)-status;
      trace("proc [%d] exited (status: %d %s)",
        p->pid, status, p->err ? err_str(p->err) : "ok");
    }
  #else
    // wait for process-group leader
    int status = 0;
    if UNLIKELY(waitpid(p->pid, &status, 0) == -1) {
      p->err = err_errno();
      goto end;
    }
    if (status != 0)
      p->err = status < 0 ? ErrInvalid : (err_t)-status;

    // wait for other process in the group
    while (waitpid(-p->pid, NULL, 0) != -1) {
    }
    if UNLIKELY(errno != ECHILD && p->err == 0)
      p->err = err_errno();
end:
  #endif

  subproc_close(p);
  return p->err;
}


err_t subproc_spawn(
  subproc_t* p,
  const char* restrict exefile,
  char*const* restrict argv,
  char*const* restrict nullable envp,
  const char* restrict nullable cwd)
{
  extern char **environ;
  err_t err = 0;
  int fd[2];

  if (!envp)
    envp = environ;

  if (pipe(fd) < 0)
    return err_errno();

  #define CHECKERR(expr) ((err = err_errnox(expr)))

  posix_spawnattr_t attrs;
  if CHECKERR( posix_spawnattr_init(&attrs) ) {
    dlog("posix_spawnattr_init failed");
    goto err0;
  }
  if CHECKERR( posix_spawnattr_setflags(&attrs, POSIX_SPAWN_SETPGROUP) ) {
    dlog("posix_spawnattr_setflags failed");
    goto err0;
  }

  posix_spawn_file_actions_t actions;
  if CHECKERR( posix_spawn_file_actions_init(&actions) ) {
    dlog("posix_spawn_file_actions_init failed");
    goto err1;
  }
  if CHECKERR( posix_spawn_file_actions_addclose(&actions, fd[0]) ) {
    dlog("posix_spawn_file_actions_addclose failed");
    goto err2;
  }
  // Note: pid=0 makes setpgid make the current process the group leader
  if CHECKERR( posix_spawnattr_setpgroup(&actions, 0) ) {
    dlog("posix_spawnattr_setpgroup failed");
    goto err2;
  }

  #ifdef HAS_SPAWN_ADDCHDIR
    if (cwd && CHECKERR( posix_spawn_file_actions_addchdir_np(&actions, cwd) )) {
      dlog("posix_spawn_file_actions_addchdir_np failed");
      goto err2;
    }
  #else
    char prev_cwd[PATH_MAX];
    getcwd(prev_cwd, sizeof(prev_cwd));
    if (cwd && chdir(cwd) < 0) {
      dlog("chdir(%s) failed", cwd);
      err = err_errno();
      goto err2;
    }
  #endif

  pid_t pid;
  if CHECKERR( posix_spawn(&pid, exefile, &actions, &attrs, argv, environ) ) {
    dlog("posix_spawn(%s ...) failed", exefile);
    goto err2;
  }
  trace("proc[%d] spawned", pid);
  subproc_open(p, pid);
  posix_spawn_file_actions_destroy(&actions);
  close(fd[1]);

  #ifndef HAS_SPAWN_ADDCHDIR
    if (cwd && chdir(prev_cwd) < 0) {
      dlog("chdir(%s) failed", prev_cwd);
      err = err_errno();
      goto err2;
    }
  #endif

  #undef CHECKERR

  return 0;

err2:
  posix_spawn_file_actions_destroy(&actions);
err1:
  posix_spawnattr_destroy(&attrs);
err0:
  close(fd[0]);
  close(fd[1]);
  return err;
}


err_t _subproc_fork(
  subproc_t* p,
  subproc_fork_t fn,
  const char* nullable cwd,
  uintptr a, uintptr b, uintptr c, uintptr d, uintptr e, uintptr f)
{
  #define RETURN_ON_ERROR(LIBC_CALL) \
    if UNLIKELY((LIBC_CALL) == -1) { \
      warn(#LIBC_CALL); \
      return ErrCanceled; \
    }

  #define EXIT_ON_ERROR(LIBC_CALL) \
    if UNLIKELY((LIBC_CALL) == -1) \
      err(1, #LIBC_CALL) \

  int fds[2];
  pid_t pid;

  RETURN_ON_ERROR( pipe(fds) );
  RETURN_ON_ERROR( (pid = fork()) );
  if (pid == 0) {
    // child process

    if (cwd && *cwd)
      chdir(cwd);

    // create process group and communicate the fact to the parent
    EXIT_ON_ERROR( setpgrp() );
    EXIT_ON_ERROR( close(fds[0]) );
    EXIT_ON_ERROR( write(fds[1], &(u8[1]){0}, sizeof(u8)) );
    EXIT_ON_ERROR( close(fds[1]) );

    err_t err = fn(a, b, c, d, e, f);
    int status = -(int)err;
    _exit(status);
    UNREACHABLE;
  }

  // wait until the child has created its own process group
  close(fds[1]);
  u8 tmp;
  read(fds[0], &tmp, sizeof(u8));
  close(fds[0]);

  trace("proc [%d] (fork of %d) spawned", pid, getpid());

  #undef RETURN_ON_ERROR
  #undef EXIT_ON_ERROR

  subproc_open(p, pid);
  return 0;
}


void subprocs_cancel(subprocs_t* sp) {
  for (u32 i = 0; i < sp->cap; i++) {
    if (sp->procs[i].pid)
      kill(sp->procs[i].pid, /*SIGINT*/2);
  }
  if (sp->promise)
    sp->promise->await = NULL;
  mem_freet(sp->ma, sp);
}


static err_t _subprocs_await(subprocs_t* sp, u32 maxcount) {
  err_t err = ErrEnd;

  for (u32 i = 0; i < sp->cap; i++) {
    subproc_t* proc = &sp->procs[i];

    if (proc->pid == 0)
      continue;

    err_t err1 = subproc_await(proc);
    if UNLIKELY(err1) {
      if (err1)
        dlog("subproc_await error %d", err1);
      if (!err)
        err = err1;
    } else {
      err = 0;
    }

    if (maxcount-- == 1)
      break;
  }
  return err;
}


err_t subprocs_await(subprocs_t* sp) {
  err_t err = _subprocs_await(sp, U32_MAX);
  if (sp->promise)
    sp->promise->await = NULL;
  mem_freet(sp->ma, sp);
  return err;
}


err_t subprocs_await_one(subprocs_t* sp) {
  return _subprocs_await(sp, 1);
}


subproc_t* nullable subprocs_alloc(subprocs_t* sp) {
  // select the first unused proc
  for (u32 i = 0; i < sp->cap; i++) {
    subproc_t* proc = &sp->procs[i];
    if (proc->pid == 0) {
      memset(proc, 0, sizeof(*proc));
      return proc;
    }
  }

  // saturated; wait for a process to finish
  trace("subprocs_alloc waiting for subprocs_await_one");
  err_t err = subprocs_await_one(sp);
  if (err) {
    trace("subprocs_await_one error %d", err);
    return NULL;
  }

  // try again
  return subprocs_alloc(sp);
}


static err_t subprocs_promise_await(void* ptr) {
  return subprocs_await(ptr);
}


subprocs_t* nullable subprocs_create_promise(memalloc_t ma, promise_t* dst_p) {
  subprocs_t* sp = mem_alloct(ma, subprocs_t);
  if (!sp)
    return NULL;
  sp->ma = ma;
  sp->promise = dst_p;
  sp->cap = comaxproc;
  sp->procs = mem_alloctv(ma, subproc_t, sp->cap);
  if (!sp->procs) {
    mem_freet(ma, sp);
    return NULL;
  }

  dst_p->impl = sp;
  dst_p->await = subprocs_promise_await;
  return sp;
}
