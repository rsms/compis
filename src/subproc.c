// SPDX-License-Identifier: Apache-2.0
#include "colib.h"
#include "subproc.h"

#include <sys/wait.h> // waitpid
#include <errno.h> // ECHILD
#include <unistd.h> // fork
#include <err.h>
#include <signal.h>


#define trace(fmt, va...) _trace(opt_trace_subproc, 3, "subproc", fmt, ##va)


static pid_t g_parent_pid = 0;


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

  #ifdef __APPLE__
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


err_t _subproc_spawn(
  subproc_t* p,
  _subproc_spawn_t fn,
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
  RETURN_ON_ERROR( pipe(fds) );

  pid_t pid;
  RETURN_ON_ERROR( (pid = fork()) );

  if (pid == 0) {
    // child process
    g_parent_pid = getpid();
    trace("proc [%d] spawned (group leader)", g_parent_pid);

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

  #undef RETURN_ON_ERROR
  #undef EXIT_ON_ERROR

  subproc_open(p, pid);
  return 0;
}


err_t _subproc_spawn_child(
  _subproc_spawn_t fn,
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

  assertf(g_parent_pid > 0, "not in parent subproc_spawn");

  pid_t pid;
  RETURN_ON_ERROR( (pid = fork()) );

  if (pid == 0) {
    // child process
    trace("proc [%d] spawned (child of [%d])", getpid(), g_parent_pid);

    err_t status = fn(a, b, c, d, e, f);
    _exit(-(int)status);
    UNREACHABLE;
  }

  #undef RETURN_ON_ERROR
  #undef EXIT_ON_ERROR

  return 0;
}


void subprocs_cancel(subprocs_t* sp) {
  for (u32 i = 0; i < sp->len; i++)
    kill(sp->v[i].pid, /*SIGINT*/2);
  if (sp->promise)
    sp->promise->await = NULL;
  sp->len = 0; // in case of use after free
  mem_freet(sp->ma, sp);
}


err_t subprocs_await(subprocs_t* sp) {
  err_t err = 0;
  for (u32 i = 0; i < sp->len; i++) {
    err_t err1 = subproc_await(&sp->v[i]);
    if (!err)
      err = err1;
  }
  sp->len = 0; // in case of use after free
  if (sp->promise)
    sp->promise->await = NULL;
  mem_freet(sp->ma, sp);
  return err;
}


subproc_t* nullable subprocs_alloc(subprocs_t* sp) {
  if (sp->len == countof(sp->v))
    return NULL;
  return &sp->v[sp->len++];
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
  dst_p->impl = sp;
  dst_p->await = subprocs_promise_await;
  return sp;
}
