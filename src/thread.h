// threads and atomic memory operations
// SPDX-License-Identifier: Apache-2.0
#pragma once

// atomics
#if defined(__STDC_NO_ATOMICS__)
  #error "TODO: no stdatomic.h"
#else
  #undef __STDC_HOSTED__
  #include <stdatomic.h> // compiler-rt

  #define AtomicLoad(x, order) atomic_load_explicit((x), (order))
  #define AtomicLoadAcq(x)     atomic_load_explicit((x), memory_order_acquire)

  #define AtomicStore(x, v, order) atomic_store_explicit((x), (v), (order))
  #define AtomicStoreRel(x, v)     atomic_store_explicit((x), (v), memory_order_release)

  // note: these operations return the previous value; _before_ applying the operation
  #define AtomicAdd(x, n, order) atomic_fetch_add_explicit((x), (n), (order))
  #define AtomicSub(x, n, order) atomic_fetch_sub_explicit((x), (n), (order))
  #define AtomicOr(x, n, order)  atomic_fetch_or_explicit((x), (n), (order))
  #define AtomicAnd(x, n, order) atomic_fetch_and_explicit((x), (n), (order))
  #define AtomicXor(x, n, order) atomic_fetch_xor_explicit((x), (n), (order))

  // Compare And Swap
  #define AtomicCAS(p, oldval, newval, order_succ, order_fail) \
    atomic_compare_exchange_strong_explicit( \
      (p), (oldval), (newval), (order_succ), (order_fail))

  #define AtomicCASRel(p, oldval, newval) \
    AtomicCAS((p), (oldval), (newval), memory_order_release, memory_order_relaxed)

  #define AtomicCASAcqRel(p, oldval, newval) \
    AtomicCAS((p), (oldval), (newval), memory_order_acq_rel, memory_order_relaxed)

  // The weak forms of AtomicCAS is allowed to fail spuriously, that is,
  // act as if *obj != *expected even if they are equal. When a compare-and-exchange
  // is in a loop, the weak version will yield better performance on some platforms.
  #define AtomicCASWeak(p, oldval, newval, order_succ, order_fail) \
    atomic_compare_exchange_weak_explicit( \
      (p), (oldval), (newval), (order_succ), (order_fail))

  #define AtomicCASRelaxed(p, oldval, newval) \
    AtomicCASWeak((p), (oldval), (newval), memory_order_relaxed, memory_order_relaxed)

  #define AtomicCASWeakAcq(p, oldval, newval) \
    AtomicCASWeak((p), (oldval), (newval), memory_order_acquire, memory_order_relaxed)

  #define AtomicCASWeakRelAcq(p, oldval, newval) \
    AtomicCASWeak((p), (oldval), (newval), memory_order_release, memory_order_acquire)

  // T AtomicExchange(volatile T* p, T desired, memory_order)
  // Returns the previous value
  #define AtomicExchange(p, desired_next_value, order) \
    atomic_exchange_explicit((p), (desired_next_value), (order))
#endif

// thread API
#if defined(WIN32)
  #error TODO
#elif __STDC_NO_THREADS__
  #define CO_THREAD_PTHREAD
  #include <pthread.h>
#else
  #define CO_THREAD_C11
  #include <threads.h>
#endif

// semaphore API
#if defined(WIN32) || defined(__MACH__)
  typedef uintptr sema_t;
#elif defined(__unix__)
  #include <semaphore.h>
  #define CO_SEMAPHORE_POSIX
#else
  #error TODO
#endif

// cpu_yield() yields for other work on a CPU core
#if defined(__i386) || defined(__i386__) || defined(__x86_64__)
  #define cpu_yield() __asm__ __volatile__("pause")
#elif defined(__arm__) || defined(__arm64__) || defined(__aarch64__)
  #define cpu_yield() __asm__ __volatile__("yield")
#elif defined(mips) || defined(__mips__) || defined(MIPS) || defined(_MIPS_) || defined(__mips64)
  #if defined(_ABI64) && (_MIPS_SIM == _ABI64)
    #define cpu_yield() __asm__ __volatile__("pause")
  #else
    // comment from WebKit source:
    //   The MIPS32 docs state that the PAUSE instruction is a no-op on older
    //   architectures (first added in MIPS32r2). To avoid assembler errors when
    //   targeting pre-r2, we must encode the instruction manually.
    #define cpu_yield() __asm__ __volatile__(".word 0x00000140")
  #endif
#elif defined(WIN32)
  #include <immintrin.h>
  #define cpu_yield() _mm_pause()
#elif defined(RSM_NO_LIBC)
  #define cpu_yield() ((void)0)
#else
  // GCC & clang intrinsic
  #define cpu_yield() __builtin_ia32_pause()
#endif

// thread_yield() yields for other threads to be scheduled on the current CPU by the OS
#if defined(WIN32)
  #define thread_yield() cpu_yield()
#elif defined(RSM_NO_LIBC)
  #define thread_yield() ((void)0)
#else
  #include <sched.h>
  #define thread_yield() sched_yield() // equivalent to thrd_yield
#endif


ASSUME_NONNULL_BEGIN


// thrd_t is an OS thread
#ifdef CO_THREAD_PTHREAD
  // c11 thread shim for pthreads
  typedef pthread_t thrd_t;
  typedef int (*thrd_start_t)(void*);
  enum { thrd_success, thrd_timedout, thrd_busy, thrd_error, thrd_nomem };
  int    thrd_create(thrd_t* thr, thrd_start_t func, void* arg);
  void   thrd_exit(int res);
  int    thrd_join(thrd_t thr, int* res);
  int    thrd_detach(thrd_t thr);
  thrd_t thrd_current(void);
  int    thrd_equal(thrd_t a, thrd_t b);
  int    thrd_sleep(const struct timespec* ts_in, struct timespec* rem_out);
  void   thrd_yield(void);
#endif


// mutex_t is a regular mutex
typedef struct {
  #ifdef CO_THREAD_PTHREAD
    pthread_mutex_t m;
  #elif defined(CO_THREAD_C11)
    mtx_t m;
  #endif
  _Atomic(u32) w; // writer count
  _Atomic(u32) r; // reader count (only used by rwmutex, here for compactness)
} mutex_t;
err_t mutex_init(mutex_t*);
void mutex_dispose(mutex_t*);
static void mutex_lock(mutex_t*);
static bool mutex_trylock(mutex_t*);
static void mutex_unlock(mutex_t*);
static bool mutex_islocked(mutex_t*);


// rwmutex_t supports multiple concurrent readers when there are no writers.
// There can be many concurrent readers but only one writer.
// While no write lock is held, up to 16777214 read locks may be held.
// While a write lock is held no read locks or other write locks can be held.
typedef struct { mutex_t m; } rwmutex_t;
err_t rwmutex_init(rwmutex_t*);
void rwmutex_dispose(rwmutex_t*);
void rwmutex_rlock(rwmutex_t*);     // acquire read-only lock (blocks until acquired)
bool rwmutex_tryrlock(rwmutex_t*);  // attempt to acquire read-only lock (non-blocking)
void rwmutex_runlock(rwmutex_t*);   // release read-only lock
void rwmutex_lock(rwmutex_t*);      // acquire excludive lock (blocks until acquired)
bool rwmutex_trylock(rwmutex_t*);   // attempt to acquire excludive lock (non-blocking)
void rwmutex_unlock(rwmutex_t*);    // release excludive lock
static bool rwmutex_islocked(rwmutex_t*);  // test if locked for reading and writing
static bool rwmutex_isrlocked(rwmutex_t*); // test if locked for reading (not writing)


// sema_t is a portable semaphore; a thin layer over the OS's semaphore implementation.
#ifdef CO_SEMAPHORE_POSIX
  typedef sem_t sema_t;
#else
  typedef uintptr sema_t;
#endif
err_t sema_init(sema_t*, u32 initcount); // returns false if system impl failed (rare)
void sema_dispose(sema_t*);
bool sema_wait(sema_t*);    // wait for a signal
bool sema_trywait(sema_t*); // try acquire a signal; return false instead of blocking
bool sema_timedwait(sema_t*, u64 timeout_usecs);
bool sema_signal(sema_t*, u32 count /*must be >0*/);


// hcsema_t is a semaphore which is more efficient than sema_t under
// high-contention condition, by avoiding syscalls.
// Waiting when there's already a signal available is extremely cheap and involves
// no syscalls. If there's no signal the implementation will retry by spinning for
// a short while before eventually falling back to sema_t.
typedef struct {
  _Atomic(isize) count;
  sema_t         sema;
} hcsema_t;

err_t hcsema_init(hcsema_t*, u32 initcount); // returns false if system impl failed (rare)
void hcsema_dispose(hcsema_t*);
bool hcsema_wait(hcsema_t*);
bool hcsema_trywait(hcsema_t*);
bool hcsema_timedwait(hcsema_t*, u64 timeout_usecs);
void hcsema_signal(hcsema_t*, u32 count /*must be >0*/);
usize hcsema_approxavail(hcsema_t*);


// spinmutex_t is a mutex that will spin for a short while and then block
typedef struct {
  _Atomic(bool) flag;
  _Atomic(i32)  nwait;
  sema_t        sema;
} spinmutex_t;
static err_t spinmutex_init(spinmutex_t* m); // returns false if system failed to init semaphore
static void spinmutex_dispose(spinmutex_t* m);
static void spinmutex_lock(spinmutex_t* m);
static void spinmutex_unlock(spinmutex_t* m);


//———————————————————————————————————————————————————————————————————————————————————————
// inline impl

#ifdef CO_THREAD_C11
  #define _mutex_lock(mu)    (mtx_lock(&(mu)->m) == 0)
  #define _mutex_unlock(mu)  (mtx_unlock(&(mu)->m) == 0)
#elif defined(CO_THREAD_PTHREAD)
  #define _mutex_lock(mu)    (pthread_mutex_lock(&(mu)->m) == 0)
  #define _mutex_unlock(mu)  (pthread_mutex_unlock(&(mu)->m) == 0)
#endif

inline static void mutex_lock(mutex_t* mu) {
  if UNLIKELY(AtomicAdd(&mu->w, 1, memory_order_seq_cst))
    safecheckxf(_mutex_lock(mu), "mutex_lock");
}

inline static void mutex_unlock(mutex_t* mu) {
  if UNLIKELY(AtomicSub(&mu->w, 1, memory_order_seq_cst) > 1)
    safecheckxf(_mutex_unlock(mu), "mutex_unlock");
}

inline static bool mutex_trylock(mutex_t* mu) {
  u32 w = 0;
  return AtomicCAS(&mu->w, &w, 1, memory_order_seq_cst, memory_order_relaxed);
}

inline static bool mutex_islocked(mutex_t* mu) {
  return AtomicLoadAcq(&mu->w) > 0;
}

inline static bool rwmutex_isrlocked(rwmutex_t* mu) {
  return AtomicLoadAcq(&mu->m.r) > 0;
}

inline static bool rwmutex_islocked(rwmutex_t* mu) {
  return mutex_islocked(&mu->m);
}


void _spinmutex_wait(spinmutex_t* m);

inline static err_t spinmutex_init(spinmutex_t* m) {
  m->flag = false;
  m->nwait = 0;
  return sema_init(&m->sema, 0);
}

inline static void spinmutex_dispose(spinmutex_t* m) {
  sema_dispose(&m->sema);
}

inline static void spinmutex_lock(spinmutex_t* m) {
  if (AtomicExchange(&m->flag, true, memory_order_acquire))
    _spinmutex_wait(m); // already locked -- slow path
}

inline static void spinmutex_unlock(spinmutex_t* m) {
  AtomicExchange(&m->flag, false, memory_order_seq_cst);
  if (AtomicLoad(&m->nwait, memory_order_seq_cst) != 0) {
    // at least one thread waiting on a semaphore signal -- wake one thread
    sema_signal(&m->sema, 1); // TODO: should we check the return value?
  }
}

ASSUME_NONNULL_END
