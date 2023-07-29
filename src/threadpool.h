// SPDX-License-Identifier: Apache-2.0
/*
Example that uses a channel to wait for completion and send back results:
  static void my_work_fun(chan_t* ch, int a, int b, int c) {
    int sum = a + b + c;
    chan_send(ch, &sum);
  }
  void example() {
    chan_t* ch = chan_open(memalloc_default(), sizeof(int), 0);
    err_t err = threadpool_submit(my_work_fun, ch, 2, 3, 4);
    // ...
    int sum;
    bool ok = chan_recv(ch, &sum);
    printf("a + b + c = %d\n", sum);
    chan_close(ch);
    chan_free(ch);
  }

Example of "fire and forget":
  static void my_work_fun(int a, int b, int c) {
    printf("a + b + c = %d\n", a + b + c);
  }
  void example() {
    err_t err = threadpool_submit(my_work_fun, 2, 3, 4);
    // ...
  }

*/
#pragma once
#include "future.h"
#include "chan.h"
ASSUME_NONNULL_BEGIN

// threadpool_submit enqueues fn to be called on a thread in the pool along with
// a channel for completion results and optional arguments.
// Returns ErrEnd if threadpool_stop has been called.
// Returns ErrNotSupported if comaxproc==1 or threadpool_init failed to mem_alloc.
err_t threadpool_submit(void(*)(/*arg void* ...*/), ...);

err_t threadpool_init();

//———————————————————————————————————————————————————————————————————————————————————————
// implementation

typedef void(*_threadpool_fun_t)(
  const void* a, const void* b, const void* c, const void* d, const void* e);

err_t _threadpool_submit(
  _threadpool_fun_t,
  const void* nullable a, const void* nullable b, const void* nullable c,
  const void* nullable d, const void* nullable e);

#define threadpool_submit(fn, ...) \
  __VARG_DISP(_threadpool_submit, fn, ##__VA_ARGS__)

#define _threadpool_arg(x) _Generic((x), \
  i8:      (void*)(uintptr)(x), \
  u8:      (void*)(uintptr)(x), \
  i16:     (void*)(uintptr)(x), \
  u16:     (void*)(uintptr)(x), \
  i32:     (void*)(uintptr)(x), \
  u32:     (void*)(uintptr)(x), \
  i64:     (void*)(uintptr)(x), \
  u64:     (void*)(uintptr)(x), \
  uintptr: (void*)(uintptr)(x), \
  default: (x) \
)

#define _threadpool_submit1(fn) \
  _threadpool_submit(((_threadpool_fun_t)(fn)), \
    NULL,NULL,NULL,NULL,NULL)

#define _threadpool_submit2(fn, a) \
  _threadpool_submit(((_threadpool_fun_t)(fn)), \
    _threadpool_arg(a), \
    NULL,NULL,NULL,NULL)

#define _threadpool_submit3(fn, a, b) \
  _threadpool_submit(((_threadpool_fun_t)(fn)), \
    _threadpool_arg(a), \
    _threadpool_arg(b), \
    NULL,NULL,NULL)

#define _threadpool_submit4(fn, a, b, c) \
  _threadpool_submit(((_threadpool_fun_t)(fn)), \
    _threadpool_arg(a), \
    _threadpool_arg(b), \
    _threadpool_arg(c), \
    NULL,NULL)

#define _threadpool_submit5(fn, a, b, c, d) \
  _threadpool_submit(((_threadpool_fun_t)(fn)), \
    _threadpool_arg(a), \
    _threadpool_arg(b), \
    _threadpool_arg(c), \
    _threadpool_arg(d), \
    NULL)

#define _threadpool_submit6(fn, a, b, c, d, e) \
  _threadpool_submit(((_threadpool_fun_t)(fn)), \
    _threadpool_arg(a), \
    _threadpool_arg(b), \
    _threadpool_arg(c), \
    _threadpool_arg(d), \
    _threadpool_arg(e))


ASSUME_NONNULL_END
