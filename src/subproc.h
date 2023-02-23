// subprocess management
// SPDX-License-Identifier: Apache-2.0
#pragma once
ASSUME_NONNULL_BEGIN

typedef struct {
  pid_t pid;
  err_t err;
} subproc_t;

typedef struct {
  memalloc_t          ma;
  subproc_t*          procs;
  u32                 cap;
  promise_t* nullable promise;
} subprocs_t;

void subproc_open(subproc_t* p, pid_t pid);
void subproc_close(subproc_t* p);
err_t subproc_await(subproc_t* p);

//inline static bool subproc_isresolved(const subproc_t* p) { return p->pid == 0; }

subprocs_t* nullable subprocs_create_promise(memalloc_t ma, promise_t* dst_p);
subproc_t* nullable subprocs_alloc(subprocs_t* sp);
err_t subprocs_await(subprocs_t* sp);
void subprocs_cancel(subprocs_t* sp);

err_t subproc_spawn(
  subproc_t* p,
  const char* restrict exefile,
  char*const* restrict argv,
  char*const* restrict nullable envp,
  const char* restrict nullable cwd);

// subproc_fork forks the process and calls fn bound to subproc p in a new process group.
// 1. calls fork()
// 2. calls chdir(cwd) if cwd is not NULL and not ""
// 3. calls fn(args...)
// 4. calls _exit( - return_value_from_fn)
//
// err_t subproc_fork(subproc_t* p, const char* cwd, f(...uintptr), ...uintptr)
#define subproc_fork(p, fn, cwd, ...) \
  __VARG_CONCAT(_subproc_fork,__VARG_NARGS(__VA_ARGS__))( \
    (p), (subproc_fork_t)(fn), (cwd), __VA_ARGS__)

typedef err_t(*subproc_fork_t)(
  uintptr a, uintptr b, uintptr c, uintptr d, uintptr e, uintptr f);

err_t _subproc_fork(subproc_t* p, subproc_fork_t fn, const char* nullable cwd,
  uintptr a, uintptr b, uintptr c, uintptr d, uintptr e, uintptr f);

#define _subproc_fork6(p, fn, cwd, a, b, c, d, e, f) _subproc_fork( \
  (p), (subproc_fork_t)(fn), (cwd), \
  (uintptr)(a), (uintptr)(b), (uintptr)(c), (uintptr)(d), (uintptr)(e), (uintptr)(f))
#define _subproc_fork5(p, fn, cwd, a, b, c, d, e) _subproc_fork( \
  (p), (subproc_fork_t)(fn), (cwd), \
  (uintptr)(a), (uintptr)(b), (uintptr)(c), (uintptr)(d), (uintptr)(e), 0)
#define _subproc_fork4(p, fn, cwd, a, b, c, d) _subproc_fork( \
  (p), (subproc_fork_t)(fn), (cwd), \
  (uintptr)(a), (uintptr)(b), (uintptr)(c), (uintptr)(d), 0, 0)
#define _subproc_fork3(p, fn, cwd, a, b, c) _subproc_fork( \
  (p), (subproc_fork_t)(fn), (cwd), \
  (uintptr)(a), (uintptr)(b), (uintptr)(c), 0, 0, 0)
#define _subproc_fork2(p, fn, cwd, a, b) _subproc_fork( \
  (p), (subproc_fork_t)(fn), (cwd),\
  (uintptr)(a), (uintptr)(b), 0, 0, 0, 0)
#define _subproc_fork1(p, fn, cwd, a) _subproc_fork( \
  (p), (subproc_fork_t)(fn), (cwd),\
  (uintptr)(a), 0, 0, 0, 0, 0)
#define _subproc_fork0(p, fn, cwd) _subproc_fork( \
  (p), (subproc_fork_t)(fn), (cwd), 0, 0, 0, 0, 0, 0)


ASSUME_NONNULL_END
