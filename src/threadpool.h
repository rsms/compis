// SPDX-License-Identifier: Apache-2.0
/*
Example that uses a channel to wait for completion and send back results:

  static void my_work_fun(u32 argc, uintptr argv[]) {
    chan_t* ch = (chan_t*)argv[0];
    int sum = 0;
    for (u32 i = 1; i < argc; i++)
      sum += (int)(uintptr)argv[i];
    chan_send(ch, &sum);
  }

  void example() {
    chan_t* ch = chan_open(memalloc_default(), sizeof(int), 0);
    err_t err = threadpool_submit(my_work_fun, ch, (uintptr)2, (uintptr)3);
    // ...
    int sum;
    bool ok = chan_recv(ch, &sum);
    assert(sum == 2 + 3);
    chan_close(ch);
    chan_free(ch);
  }

Example of "fire and forget":

  static void my_work_fun(u32 argc, uintptr argv[]) {
    int sum = 0;
    for (u32 i = 0; i < argc; i++)
      sum += (int)(uintptr)argv[i];
    printf("sum: %d\n", sum);
  }

  void example() {
    err_t err = threadpool_submit(my_work_fun, (uintptr)2, (uintptr)3);
    // ...
  }

*/
#pragma once
#include "future.h"
#include "chan.h"
ASSUME_NONNULL_BEGIN

typedef void(*threadpool_fun_t)(u32 argc, void* argv[]);

// threadpool_submit enqueues fn to be called on a thread in the pool along with
// a channel for completion results and optional arguments.
// Returns ErrEnd if threadpool_stop has been called.
// Returns ErrOverflow if threadpool_submitv is called with argc>THREADPOOL_MAX_ARGS.
err_t threadpool_submit(threadpool_fun_t, /*void**/...);
err_t threadpool_submitv(threadpool_fun_t, u32 argc, void* argv[]);

err_t threadpool_init();

//———————————————————————————————————————————————————————————————————————————————————————
// implementation

#define THREADPOOL_MAX_ARGS 4

#define threadpool_submit(fn, ...) \
  __VARG_DISP(_threadpool_submit, fn, ##__VA_ARGS__)

#define _threadpool_submit1(fn) \
  threadpool_submitv((fn), 0, (void*[]){})

#define _threadpool_submit2(fn, a) \
  threadpool_submitv((fn), 1, (void*[]){(a)})

#define _threadpool_submit3(fn, a, b) \
  threadpool_submitv((fn), 2, (void*[]){(a),(b)})

#define _threadpool_submit4(fn, a, b, c) \
  threadpool_submitv((fn), 3, (void*[]){(a),(b),(c)})

#define _threadpool_submit5(fn, a, b, c, d) \
  threadpool_submitv((fn), 4, (void*[]){(a),(b),(c),(d)})


ASSUME_NONNULL_END
