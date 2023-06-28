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

// SUBPROC_USE_PGRP: define to make subprocesses leaders of their own pgroup
//#define SUBPROC_USE_PGRP

#if DEBUG
  #define log_errno(fmt, args...) warn(fmt, ##args)
#else
  #define log_errno(fmt, args...) warn("subproc")
#endif

#define trace(fmt, va...) _trace(opt_trace_subproc, 3, "subproc", fmt, ##va)


#if defined(SUBPROC_USE_PGRP) && defined(__APPLE__)
  // waitpid(-pgrp) isn't reliable on macOS/darwin.
  // Thank you to Julio Merino for these functions.
  // https://jmmv.dev/2019/11/wait-for-process-group-darwin.html
  #include <sys/event.h>
  #include <sys/sysctl.h>
  #include <sys/types.h>

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

  int status = 0;

  #if defined(SUBPROC_USE_PGRP) && defined(__APPLE__)
    // wait for process-group leader
    darwin_wait_pid(p->pid);

    // wait for other process in the group
    darwin_pgrp_wait(p->pid);

    // get leader exit status
    if (waitpid(p->pid, &status, 0) == -1 || !WIFEXITED(status)) {
      trace("proc[%d] died or experienced an error", p->pid);
      p->err = ErrCanceled;
    } else {
      status = WEXITSTATUS(status);
      if (status != 0)
        p->err = status < 0 ? ErrInvalid : (err_t)-status;
      trace("proc[%d] exited (status: %d %s)",
        p->pid, status, p->err ? err_str(p->err) : "ok");
    }
  #elif defined(SUBPROC_USE_PGRP)
    // wait for process-group leader
    if UNLIKELY(waitpid(p->pid, &status, 0) == -1) {
      p->err = errno ? err_errno() : ErrIO;
    } else {
      if (status != 0)
        p->err = status < 0 ? ErrInvalid : (err_t)-status;
      // wait for other process in the group
      while (waitpid(-p->pid, NULL, 0) != -1) {}
      if UNLIKELY(errno != ECHILD && p->err == 0)
        p->err = errno ? err_errno() : ErrCanceled;
    }
  #else // not SUBPROC_USE_PGRP
    if (waitpid(p->pid, &status, 0) < 0) {
      p->err = errno ? err_errno() : ErrIO;
      log_errno("waitpid %d", p->pid);
    } else if (WIFEXITED(status)) {
      if (WEXITSTATUS(status) != 0) {
        p->err = errno ? err_errno() : ErrCanceled;
        trace("proc[%d] failed (status %d)", p->pid, status);
      }
    } else if (WIFSIGNALED(status)) {
      p->err = errno ? err_errno() : ErrCanceled;
      trace("proc[%d] terminated due to signal %d", p->pid, WTERMSIG(status));
    } else {
      p->err = ErrCanceled;
      trace("proc[%d] terminated due to unknown cause", p->pid);
    }
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

  #define CHECKERR(expr) ((err = err_errnox(expr)))

  posix_spawn_file_actions_t actions;
  posix_spawnattr_t* attrs = NULL;

  if CHECKERR( posix_spawn_file_actions_init(&actions) ) {
    log_errno("posix_spawn_file_actions_init");
    goto err1;
  }

  #ifdef SUBPROC_USE_PGRP
    posix_spawnattr_t attrs_;
    attrs = &attrs_;
    if CHECKERR( posix_spawnattr_init(attrs) ) {
      log_errno("posix_spawnattr_init");
      goto err0;
    }
    if CHECKERR( posix_spawnattr_setflags(attrs, POSIX_SPAWN_SETPGROUP) ) {
      log_errno("posix_spawnattr_setflags");
      goto err0;
    }
    // Note: pid=0 makes setpgid make the current process the group leader
    if CHECKERR( posix_spawnattr_setpgroup(&actions, 0) ) {
      log_errno("posix_spawnattr_setpgroup");
      goto err2;
    }
  #endif

  #ifdef HAS_SPAWN_ADDCHDIR
    if (cwd && CHECKERR( posix_spawn_file_actions_addchdir_np(&actions, cwd) )) {
      log_errno("posix_spawn_file_actions_addchdir_np");
      goto err2;
    }
  #else
    char prev_cwd[PATH_MAX];
    getcwd(prev_cwd, sizeof(prev_cwd));
    if (cwd && chdir(cwd) < 0) {
      log_errno("chdir(%s)", cwd);
      err = err_errno();
      goto err2;
    }
  #endif

  pid_t pid;
  if CHECKERR( posix_spawn(&pid, exefile, &actions, attrs, argv, environ) ) {
    log_errno("posix_spawn(%s ...)", exefile);
    goto err2;
  }
  trace("proc[%d] spawned", pid);
  subproc_open(p, pid);
  posix_spawn_file_actions_destroy(&actions);

  #ifndef HAS_SPAWN_ADDCHDIR
    if (cwd && chdir(prev_cwd) < 0) {
      log_errno("chdir(%s)", prev_cwd);
      err = err_errno();
      goto err2;
    }
  #endif

  #undef CHECKERR

  return 0;

err2:
  posix_spawn_file_actions_destroy(&actions);
err1:
  if (attrs)
    posix_spawnattr_destroy(attrs);
#ifdef SUBPROC_USE_PGRP
err0:
#endif
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

  #ifdef SUBPROC_USE_PGRP
    int fds[2];
    RETURN_ON_ERROR( pipe(fds) );
  #endif

  pid_t pid;
  RETURN_ON_ERROR( (pid = fork()) );
  if (pid == 0) {
    // child process

    if (cwd && *cwd) {
      if (chdir(cwd) == -1) {
        warn("chdir(%s)", cwd);
        _exit(-(int)err_errno());
      }
    }

    // create process group and communicate the fact to the parent
    #ifdef SUBPROC_USE_PGRP
      EXIT_ON_ERROR( setpgrp() );
      EXIT_ON_ERROR( close(fds[0]) );
      EXIT_ON_ERROR( write(fds[1], &(u8[1]){0}, sizeof(u8)) );
      EXIT_ON_ERROR( close(fds[1]) );
    #endif

    err_t err = fn(a, b, c, d, e, f);
    int status = -(int)err;
    _exit(status);
    UNREACHABLE;
  }

  // wait until the child has created its own process group
  #ifdef SUBPROC_USE_PGRP
    close(fds[1]);
    u8 tmp;
    read(fds[0], &tmp, sizeof(u8));
    close(fds[0]);
  #endif

  trace("proc[%d] spawned (fork of %d)", pid, getpid());

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
  err_t err = 0;
  u32 nawait = 0;

  for (u32 i = 0; i < sp->cap; i++) {
    subproc_t* proc = &sp->procs[i];
    if (proc->pid == 0)
      continue;

    nawait++;
    err_t err1 = subproc_await(proc);
    if (err == 0)
      err = err1;

    if (maxcount-- == 1)
      break;
  }

  return nawait == 0 ? ErrEnd : err;
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
  trace("subprocs_alloc wait");
  err_t err = subprocs_await_one(sp);
  if (err) {
    // ErrEnd here if subprocs_cancel has been called
    if (err != ErrEnd)
      dlog("subprocs_await_one failed: %s", err_str(err));
    return NULL;
  }

  // try again
  return subprocs_alloc(sp);
}


static err_t subprocs_promise_await(void* ptr) {
  return subprocs_await(ptr);
}


subprocs_t* nullable subprocs_create_promise(memalloc_t ma, promise_t* dst_p) {
  assertf(dst_p->await == NULL, "promise %p already initialized", dst_p);
  subprocs_t* sp = mem_alloct(ma, subprocs_t);
  if (!sp)
    return NULL;
  if (comaxproc > 4096)
    dlog("subproc: limiting maxproc to 4096");
  sp->ma = ma;
  sp->promise = dst_p;
  sp->cap = MIN(comaxproc, 4096);
  sp->procs = mem_alloctv(ma, subproc_t, sp->cap);
  if (!sp->procs) {
    mem_freet(ma, sp);
    return NULL;
  }
  dst_p->impl = sp;
  dst_p->await = subprocs_promise_await;
  return sp;
}
