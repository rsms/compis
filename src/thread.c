#include "colib.h"
#include "thread.h"

#ifndef CO_SEMAPHORE_POSIX
  #if defined(WIN32)
    #include <windows.h>
  #elif defined(__MACH__)
    #undef panic // mach/mach.h defines a function called panic()
    #include <mach/mach.h>
    // redefine panic
    #define panic(fmt, ...) _panic(__FILE__, __LINE__, __FUNCTION__, fmt, ##__VA_ARGS__)
  #else
    #error Unsupported platform
  #endif
#endif

#if defined(CO_THREAD_PTHREAD) || defined(CO_SEMAPHORE_POSIX)
  #include <errno.h>
#endif


//———————————————————————————————————————————————————————————————————————————————————
// thrd_t
//———————————————————————————————————————————————————————————————————————————————————
#ifdef CO_THREAD_PTHREAD

int thrd_create(thrd_t *thr, thrd_start_t func, void *arg) {
  int res = pthread_create(thr, 0, (void*(*)(void*))func, arg);
  if(res == 0) {
    return thrd_success;
  }
  return res == ENOMEM ? thrd_nomem : thrd_error;
}

void thrd_exit(int res) {
  pthread_exit((void*)(long)res);
}

int thrd_join(thrd_t thr, int *res) {
  void *retval;
  if (pthread_join(thr, &retval) != 0)
    return thrd_error;
  if (res)
    *res = (int)(long)retval;
  return thrd_success;
}

int thrd_detach(thrd_t thr) {
  return pthread_detach(thr) == 0 ? thrd_success : thrd_error;
}

thrd_t thrd_current(void) {
  return pthread_self();
}

int thrd_equal(thrd_t a, thrd_t b) {
  return pthread_equal(a, b);
}

int thrd_sleep(const struct timespec *ts_in, struct timespec *rem_out) {
  if(nanosleep(ts_in, rem_out) < 0) {
    if(errno == EINTR) return -1;
    return -2;
  }
  return 0;
}

void thrd_yield(void) {
  sched_yield();
}

#endif // defined(CO_THREAD_PTHREAD)
//———————————————————————————————————————————————————————————————————————————————————
// mutex_t
//———————————————————————————————————————————————————————————————————————————————————
#if defined(CO_THREAD_C11)

err_t mutex_init(mutex_t* mu) {
  mu->w = 0;
  return mtx_init(&mu->m, mtx_plain) ? ErrNoMem : 0;
}

void mutex_dispose(mutex_t* mu) {
  #if DEBUG
  if (mutex_islocked(mu)) {
    dlog("warning: mutex_dispose called on locked mutex (w=%u)", AtomicLoadAcq(&mu->w));
  }
  #endif
  mtx_destroy(&mu->m);
}


//———————————————————————————————————————————————————————————————————————————————————
#elif defined(CO_THREAD_PTHREAD)

err_t mutex_init(mutex_t* mu) {
  mu->w = 0;
  return pthread_mutex_init(&mu->m, NULL) == 0 ? 0 : ErrNoMem;
}

void mutex_dispose(mutex_t* mu) {
  int err = pthread_mutex_destroy(&mu->m);
  if (err == EBUSY) {
    dlog("warning: mutex_dispose called on locked mutex");
  } else {
    safecheckf(!err, "mutex_dispose (%d) %s", err, err_str(err_errnox(err)));
  }
}

//———————————————————————————————————————————————————————————————————————————————————
#elif !defined(CO_THREAD_C11)
  #error TODO implementation
#endif


void mutex_lock(mutex_t* mu) {
  AtomicAdd(&mu->w, 1, memory_order_seq_cst);
  safecheckxf(_mutex_lock(mu), "mutex_lock");
}

void mutex_unlock(mutex_t* mu) {
  UNUSED u32 w = AtomicSub(&mu->w, 1, memory_order_seq_cst);
  assertf(w > 0, "unbalanced mutex_unlock");
  safecheckxf(_mutex_unlock(mu), "mutex_unlock");
}


//———————————————————————————————————————————————————————————————————————————————————
// rwmutex_t

// RWMUTEX_WATERMARK: this is a watermark value for mutex.r
//   mutex.r == 0                 -- no read or write locks
//   mutex.r <  RWMUTEX_WATERMARK -- mutex.r read locks
//   mutex.r >= RWMUTEX_WATERMARK -- write lock held
// rwmutex_rlock optimistically increments mutex.r thus the value of mutex.r
// may exceed RWMUTEX_WATERMARK for brief periods of time while a rwmutex_rlock fails.
const u32 RWMUTEX_WATERMARK = 0xffffff;


err_t rwmutex_init(rwmutex_t* mu) {
  mu->m.r = 0;
  return mutex_init(&mu->m);
}

void rwmutex_dispose(rwmutex_t* mu) {
  mutex_dispose(&mu->m);
}

void rwmutex_rlock(rwmutex_t* m) {
  while (1) {
    u32 r = AtomicAdd(&m->m.r, 1, memory_order_acquire);
    if (r < RWMUTEX_WATERMARK)
      return;
    // there's a write lock; revert addition and await write lock
    AtomicSub(&m->m.r, 1, memory_order_release);
    mutex_lock(&m->m);
    mutex_unlock(&m->m);
    // try read lock again
  }
}

bool rwmutex_tryrlock(rwmutex_t* m) {
  u32 r = AtomicAdd(&m->m.r, 1, memory_order_acquire);
  if (r < RWMUTEX_WATERMARK)
    return true;
  // there's a write lock; revert addition and await write lock
  AtomicSub(&m->m.r, 1, memory_order_release);
  return false;
}

void rwmutex_runlock(rwmutex_t* m) {
  while (1) {
    u32 prevr = AtomicLoadAcq(&m->m.r);
    safecheckf(prevr != 0, "no read lock held");
    if (prevr < RWMUTEX_WATERMARK) {
      AtomicSub(&m->m.r, 1, memory_order_release);
      return;
    }
    // await write lock
    mutex_lock(&m->m);
    mutex_unlock(&m->m);
  }
}

void rwmutex_lock(rwmutex_t* m) {
  int retry = 0;
  while (1) {
    u32 prevr = AtomicLoadAcq(&m->m.r);
    if (prevr == 0 && AtomicCASWeakRelAcq(&m->m.r, &prevr, RWMUTEX_WATERMARK)) {
      // no read locks; acquire write lock
      return mutex_lock(&m->m);
    }
    // spin while there are read locks
    if (retry++ == 100) {
      retry = 0;
      thread_yield();
    }
  }
}

bool rwmutex_trylock(rwmutex_t* m) {
  u32 prevr = AtomicLoadAcq(&m->m.r);
  if (prevr == 0 && AtomicCASWeakRelAcq(&m->m.r, &prevr, RWMUTEX_WATERMARK)) {
    // no read locks; acquire write lock
    return mutex_trylock(&m->m);
  }
  // read-locked
  return false;
}


void rwmutex_unlock(rwmutex_t* m) {
  int retry = 0;
  while (1) {
    u32 prevr = AtomicLoadAcq(&m->m.r);
    if (prevr >= RWMUTEX_WATERMARK &&
        AtomicCASWeakRelAcq(&m->m.r, &prevr, prevr - RWMUTEX_WATERMARK))
    {
      return mutex_unlock(&m->m);
    }
    safecheckf(prevr >= RWMUTEX_WATERMARK, "no write lock held");
    // spin
    if (retry++ == 100) {
      retry = 0;
      thread_yield();
    }
  }
}


// The value of kYieldProcessorTries is cargo culted from TCMalloc, Windows
// critical section defaults, WebKit, etc.
#define kYieldProcessorTries 1000


void _spinmutex_wait(spinmutex_t* m) {
  while (1) {
    if (!AtomicExchange(&m->flag, true, memory_order_acquire))
      break;
    usize n = kYieldProcessorTries;
    while (AtomicLoad(&m->flag, memory_order_relaxed)) {
      if (--n == 0) {
        AtomicAdd(&m->nwait, 1, memory_order_relaxed);
        while (AtomicLoad(&m->flag, memory_order_relaxed))
          sema_wait(&m->sema);
        AtomicSub(&m->nwait, 1, memory_order_relaxed);
      } else {
        // avoid starvation on hyper-threaded CPUs
        cpu_yield();
      }
    }
  }
}


//——————————————————————————————————————————————————————————————————————————————————————
// little embedded test program
#if 0

#include "hash.h"

typedef struct {
  mutex_t* mu;
  thrd_t   t;
} test_thread_t;

static int test_thread(test_thread_t* t) {
  mutex_lock(t->mu);
  dlog("test_thread %p", t->t);
  microsleep(fastrand() % 1000);
  mutex_unlock(t->mu);
  return 0;
}

__attribute__((constructor)) static void test() {
  test_thread_t threads[8];

  mutex_t mu;
  mutex_init(&mu);

  for (usize i = 0; i < countof(threads); i++) {
    test_thread_t* t = &threads[i];
    t->mu = &mu;
    int err = thrd_create(&t->t, (thrd_start_t)test_thread, t);
    assert(err == 0);
  }

  for (usize i = 0; i < countof(threads); i++) {
    test_thread_t* t = &threads[i];
    int result = 123;
    int err = thrd_join(t->t, &result);
    assert(err == 0);
    assert(result == 0);
  }
}

#endif
