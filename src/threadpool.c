// simple globally-shared thread pool
// SPDX-License-Identifier: Apache-2.0
#include "colib.h"
#include "threadpool.h"
#include "thread.h"
#include "chan.h"


#ifdef CO_TRACE_THREADPOOL
  #define trace(fmt, va...) _dlog(6, "tpool", __FILE__, __LINE__, fmt, ##va)
#else
  #define trace(fmt, va...) ((void)0)
#endif


typedef struct {
  thrd_t t;
} worker_thread_t;


typedef struct {
  threadpool_fun_t fn;
  void*            argv[THREADPOOL_MAX_ARGS];
  u32              argc;
  #ifdef CO_TRACE_THREADPOOL
  u32              trace_id;
  #endif
} message_t;


static worker_thread_t* g_threadv = NULL;
static mutex_t          g_spawnmu;
static u32              g_threadcap = 0;
static _Atomic(u32)     g_threadlen = 0;
static chan_t*          g_workch = NULL;
static _Atomic(u32)     g_inflightcount = 0; // current workloads in process
#ifdef CO_TRACE_THREADPOOL
static _Atomic(u32)     g_trace_idgen = 0;
#endif


// SPAWN_THRESHOLD: spawn more threads when there are at least these many queued
// work requests. 1 seems like an obvious number, but some work is usually completed
// sooner than it takes to spawn a new thread and have it start accepting work.
#define SPAWN_THRESHOLD 2


#ifdef CO_TRACE_THREADPOOL
  static u32 worker_trace_id(const worker_thread_t* t) {
    u32 worker_id = 0;
    for (; worker_id < g_threadcap && t != &g_threadv[worker_id]; worker_id++) {}
    return worker_id;
  }
#endif


static int worker_thread(worker_thread_t* t) {
  message_t msg;

  #ifdef CO_TRACE_THREADPOOL
    u32 worker_id = worker_trace_id(t);
    trace("worker#%u start", worker_id);
  #endif

  // receive messages and call job functions until g_workch is closed
  for (;;) {
    if (!chan_recv(g_workch, &msg))
      break; // channel closed
    trace("worker#%u got job#%u", worker_id, msg.trace_id);
    msg.fn(msg.argc, msg.argv);
    AtomicSub(&g_inflightcount, 1, memory_order_release);
  }

  trace("worker#%u exit", worker_id);
  return 0;
}


err_t threadpool_submitv(threadpool_fun_t fn, u32 argc, void* argv[]) {
  if (argc > THREADPOOL_MAX_ARGS) {
    dlog("%s: argc(%u) > THREADPOOL_MAX_ARGS(%u)",
      __FUNCTION__, argc, THREADPOOL_MAX_ARGS);
    return ErrOverflow;
  }

  message_t msg = { .fn = fn, .argc = argc };
  memcpy(msg.argv, argv, (usize)argc * sizeof(void*));

  #ifdef CO_TRACE_THREADPOOL
    msg.trace_id = AtomicAdd(&g_trace_idgen, 1, memory_order_relaxed);
  #endif

  if UNLIKELY(!chan_send(g_workch, &msg)) {
    // channel closed
    trace("submit job#%u failed: submission channel closed", msg.trace_id);
    return ErrEnd;
  }

  trace("submit job#%u ok", msg.trace_id);

  // increment g_inflightcount
  u32 inflightcount = AtomicAdd(&g_inflightcount, 1, memory_order_acquire);

  // spawn additional thread if needed
  inflightcount++; // +1 since AtomicAdd returns previous value
  u32 threadlen = AtomicLoadAcq(&g_threadlen);

  if (inflightcount <= threadlen || (inflightcount - threadlen) < SPAWN_THRESHOLD ||
      threadlen == g_threadcap)
  {
    // etither not enough queue pressure or we have maxed out worker thread count
    return 0;
  }

  // let's try to spawn more worker threads
  mutex_lock(&g_spawnmu);

  // check if we are still over-committed (in case of race to lock g_spawnmu)
  threadlen = AtomicLoadAcq(&g_threadlen);
  inflightcount = AtomicLoadAcq(&g_inflightcount);
  if (inflightcount > threadlen && (inflightcount - threadlen) >= SPAWN_THRESHOLD &&
      threadlen < g_threadcap)
  {
    // Yup, still over-committed
    u32 newthreadlen = MIN(inflightcount, g_threadcap);
    for (u32 i = threadlen; i < newthreadlen; i++) {
      worker_thread_t* t = &g_threadv[i];
      int status = thrd_create(&t->t, (thrd_start_t)worker_thread, t);
      if UNLIKELY(status != thrd_success) {
        // treat errors gracefully; don't panic
        dlog("%s: thrd_create: %s",
          __FUNCTION__, err_str((status == thrd_nomem) ? ErrNoMem : ErrInvalid));
        newthreadlen = i;
        break;
      }
      trace("spawned extra worker#%u", worker_trace_id(t));
    }
    // update g_threadlen
    // race should not be possible here since we hold a lock on g_spawnmu
    assertf(AtomicLoadAcq(&g_threadlen) == threadlen, "race on g_threadlen");
    AtomicStore(&g_threadlen, newthreadlen, memory_order_release);
  }

  mutex_unlock(&g_spawnmu);

  return 0;
}


#ifdef CO_ENABLE_TESTS
  static void test_threadpool();
#else
  #define test_threadpool() ((void)0)
#endif


err_t threadpool_init() {
  err_t err = 0;

  // initially, start at most 4 threads
  g_threadcap = sys_ncpu();
  g_threadlen = MIN(4, g_threadcap);

  // initialize g_spawnmu
  if (( err = mutex_init(&g_spawnmu) )) {
    dlog("mutex_init failed: %s", err_str(err));
    return err;
  }

  // open a channel
  g_workch = chan_open(memalloc_default(), sizeof(message_t), g_threadcap);
  if (!g_workch) {
    elog("%s chan_open failed", __FUNCTION__);
    return ErrNoMem;
  }

  // allocate storage for worker threads
  g_threadv = mem_alloctv(memalloc_default(), worker_thread_t, g_threadcap);
  if (!g_threadv) {
    dlog("mem_alloctv(worker_thread_t, %u) failed", g_threadcap);
    g_threadcap = 0;
    return ErrNoMem;
  }

  // spawn threads
  trace("init: spawning %u threads", g_threadlen);
  for (u32 i = 0; i < g_threadlen; i++) {
    worker_thread_t* t = &g_threadv[i];
    int status = thrd_create(&t->t, (thrd_start_t)worker_thread, t);
    if (status != thrd_success) {
      if (status == thrd_nomem) {
        err = ErrNoMem;
      } else {
        err = ErrInvalid;
      }
      elog("thrd_create: %s", err_str(err));
      break;
    }
  }

  // run test (no-op unless CO_ENABLE_TESTS is defined)
  test_threadpool();

  return err;
}


/*void threadpool_stop() {
  // stop accepting work
  // this causes worker threads to exit once they are out of work
  chan_close(g_workch);

  // wait for worker threads to exit
  u32 threadlen = AtomicLoadAcq(&g_threadlen);
  for (u32 i = 0; i < threadlen; i++) {
    worker_thread_t* t = &g_threadv[i];
    int result = 123;
    int err = thrd_join(t->t, &result);
    if (err)
      dlog("%s: warning: thrd_join returned %d", __FUNCTION__, err);
    if (result != 0)
      dlog("%s: warning: worker_thread returned %d", __FUNCTION__, result);
  }

  // free memory
  chan_free(g_workch);
  mutex_dispose(&g_spawnmu);
  mem_freetv(memalloc_default(), g_threadv, g_threadcap);
}*/


//———————————————————————————————————————————————————————————————————————————————————————
#ifdef CO_ENABLE_TESTS
  static void test_work_fun(u32 argc, void* argv[]) {
    // dlog("%s: argc=%u", __FUNCTION__, argc);
    chan_t* ch = argv[0];
    uintptr result = 0;
    for (u32 i = 1; i < argc; i++) {
      // dlog("  argv[%u] = %p", i, argv[i]);
      result = i > 1 ? result*(uintptr)argv[i] : (uintptr)argv[i];
    }
    chan_send(ch, &result);
  }

  static void test_threadpool() {
    err_t err;
    bool ok;
    chan_t* ch = chan_open(memalloc_default(), sizeof(uintptr), 0);
    assertnotnull(ch);
    uintptr result, sum;

    trace("begin test");


    // submit nwork jobs to force spawning of additional threads
    u32 nwork = g_threadlen + SPAWN_THRESHOLD;
    for (u32 i = 0; i < nwork; i++) {
      err = threadpool_submit(test_work_fun, ch,
        (void*)(uintptr)2, (void*)(uintptr)3, (void*)(uintptr)4);
      assertf(!err, "%s", err_str(err));
    }

    // wait for results and sum them
    sum = 0;
    for (u32 i = 0; i < nwork; i++) {
      ok = chan_recv(ch, &result); assert(ok);
      sum += result;
    }

    assertf(sum == (uintptr)nwork * 2*3*4,
      "%lu == %lu", sum, (uintptr)nwork * 2*3*4);


    // submit work again, this time no extra threads should be spawned
    for (u32 i = 0; i < nwork; i++) {
      err = threadpool_submit(test_work_fun, ch,
        (void*)(uintptr)2, (void*)(uintptr)3, (void*)(uintptr)4);
      assertf(!err, "%s", err_str(err));
    }
    sum = 0;
    for (u32 i = 0; i < nwork; i++) {
      ok = chan_recv(ch, &result); assert(ok);
      sum += result;
    }
    assertf(sum == (uintptr)nwork * 2*3*4,
      "%lu == %lu", sum, (uintptr)nwork * 2*3*4);


    chan_close(ch);
    chan_free(ch);

    trace("end test");
    log("%s PASSED", __FUNCTION__);
  }
#else
  #define test_threadpool() ((void)0)
#endif // CO_ENABLE_TESTS
