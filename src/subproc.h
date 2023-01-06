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
  subproc_t           v[4];
  u32                 len;
  promise_t* nullable promise;
} subprocs_t;

void subproc_open(subproc_t* p, pid_t pid);
void subproc_close(subproc_t* p);
err_t subproc_await(subproc_t* p);

//inline static bool subproc_isresolved(const subproc_t* p) { return p->pid == 0; }

subproc_t* nullable subprocs_alloc(subprocs_t* sp);
subprocs_t* nullable subprocs_create_promise(memalloc_t ma, promise_t* dst_p);
err_t subprocs_await(subprocs_t* sp);
void subprocs_cancel(subprocs_t* sp);


// err_t subproc_spawn(subproc_t* p, f(...uintptr), ...uintptr)
// spawns a process bound to subproc p in a new process group
#define subproc_spawn(p, fn, ...) \
  __VARG_CONCAT(_subproc_spawn,__VARG_NARGS(__VA_ARGS__))( \
    (p), (_subproc_spawn_t)(fn), __VA_ARGS__)

// err_t subproc_spawn_child(f(...uintptr), ...uintptr)
// spawns a child process inside a parent process spawned using subproc_spawn
#define subproc_spawn_child(fn, ...) \
  __VARG_CONCAT(_subproc_spawn_child,__VARG_NARGS(__VA_ARGS__))( \
    (_subproc_spawn_t)(fn), __VA_ARGS__)

typedef err_t(*_subproc_spawn_t)(
  uintptr a, uintptr b, uintptr c, uintptr d, uintptr e, uintptr f);

err_t _subproc_spawn(subproc_t* p, _subproc_spawn_t fn,
  uintptr a, uintptr b, uintptr c, uintptr d, uintptr e, uintptr f);
#define _subproc_spawn6(p, fn, a, b, c, d, e, f) _subproc_spawn((p), \
  (_subproc_spawn_t)(fn), \
  (uintptr)(a), (uintptr)(b), (uintptr)(c), (uintptr)(d), (uintptr)(e), (uintptr)(f))
#define _subproc_spawn5(p, fn, a, b, c, d, e) _subproc_spawn((p), \
  (_subproc_spawn_t)(fn), \
  (uintptr)(a), (uintptr)(b), (uintptr)(c), (uintptr)(d), (uintptr)(e), 0)
#define _subproc_spawn4(p, fn, a, b, c, d) _subproc_spawn((p), \
  (_subproc_spawn_t)(fn), (uintptr)(a), (uintptr)(b), (uintptr)(c), (uintptr)(d), 0, 0)
#define _subproc_spawn3(p, fn, a, b, c) _subproc_spawn((p), \
  (_subproc_spawn_t)(fn), (uintptr)(a), (uintptr)(b), (uintptr)(c), 0, 0, 0)
#define _subproc_spawn2(p, fn, a, b) _subproc_spawn((p), \
  (_subproc_spawn_t)(fn), (uintptr)(a), (uintptr)(b), 0, 0, 0, 0)
#define _subproc_spawn1(p, fn, a) _subproc_spawn((p), \
  (_subproc_spawn_t)(fn), (uintptr)(a), 0, 0, 0, 0, 0)
#define _subproc_spawn0(p, fn) _subproc_spawn((p), \
  (_subproc_spawn_t)(fn), 0, 0, 0, 0, 0, 0)

err_t _subproc_spawn_child(_subproc_spawn_t fn,
  uintptr a, uintptr b, uintptr c, uintptr d, uintptr e, uintptr f);
#define _subproc_spawn_child6(fn, a, b, c, d, e, f) _subproc_spawn_child( \
  (_subproc_spawn_t)(fn), \
  (uintptr)(a), (uintptr)(b), (uintptr)(c), (uintptr)(d), (uintptr)(e), (uintptr)(f))
#define _subproc_spawn_child5(fn, a, b, c, d, e) _subproc_spawn_child( \
  (_subproc_spawn_t)(fn), \
  (uintptr)(a), (uintptr)(b), (uintptr)(c), (uintptr)(d), (uintptr)(e), 0)
#define _subproc_spawn_child4(fn, a, b, c, d) _subproc_spawn_child( \
  (_subproc_spawn_t)(fn), (uintptr)(a), (uintptr)(b), (uintptr)(c), (uintptr)(d), 0, 0)
#define _subproc_spawn_child3(fn, a, b, c) _subproc_spawn_child( \
  (_subproc_spawn_t)(fn), (uintptr)(a), (uintptr)(b), (uintptr)(c), 0, 0, 0)
#define _subproc_spawn_child2(fn, a, b) _subproc_spawn_child( \
  (_subproc_spawn_t)(fn), (uintptr)(a), (uintptr)(b), 0, 0, 0, 0)
#define _subproc_spawn_child1(fn, a) _subproc_spawn_child( \
  (_subproc_spawn_t)(fn), (uintptr)(a), 0, 0, 0, 0, 0)
#define _subproc_spawn_child0(fn) _subproc_spawn_child( \
  (_subproc_spawn_t)(fn), 0, 0, 0, 0, 0, 0)

ASSUME_NONNULL_END
