#include "colib.h"
#include "chan.h"
#include "thread.h"
//
// This implementation was inspired by the following projects, implementations and ideas:
// - golang.org/src/runtime/chan.go (main inspiration)
// - github.com/tempesta-tech/blog/blob/aaae716246041013b14ea0ae6ad287f84176b72b/
//   lockfree_rb_q.cc
// - github.com/tylertreat/chan/blob/ad341e95d785e7d38dc9de052fb18a3c35c74977/src/chan.c
// - github.com/oneapi-src/oneTBB/blob/fbc48b39c61ad1358a91c5058a8996b418518480/
//   include/oneapi/tbb/concurrent_queue.h
// - github.com/craflin/LockFreeQueue/blob/064c8d17b3032c5e9d30003798982a13508d4b49/
//   LockFreeQueueCpp11.h
// - github.com/cameron314/concurrentqueue/blob/87406493650f46ab59a534122e15cc68f4ba106b/
//   c_api/blockingconcurrentqueue.cpp
// - github.com/apple/swift-corelibs-libdispatch/blob/a181700dbf6aee3082a1c6074b3aab97560b5ef8/
//   src/queue.c
//

// DEBUG_CHAN_LOG: define to enable debug logging of send and recv
//#define DEBUG_CHAN_LOG

// DEBUG_CHAN_LOCK: define to enable debug logging of channel locks
//#define DEBUG_CHAN_LOCK

// USE_ALIGNED_ALLOC: if defined, use aligned_alloc
//#define USE_ALIGNED_ALLOC

// CACHE_LINE_SIZE is the size of a cache line of the target CPU.
// The value 64 covers i386, x86_64, arm32, arm64.
// Note that Intel TBB uses 128 (max_nfs_size).
// Must be a power of two since we use it with ALIGN2.
// TODO: set value depending on target preprocessor information.
#define CACHE_LINE_SIZE 64

#define ATTR_ALIGNED_CACHE_LINE __attribute__((aligned(CACHE_LINE_SIZE)))

// #define hbx_memory_order(name) memory_order_##name
#if !defined(__STDC_NO_ATOMICS__)
  #include <stdatomic.h>
  #define AtomicAdd(x, n, order)  atomic_fetch_add_explicit((x), (n), (order))
#else
  #error "TODO: STDC_NO_ATOMICS"
#endif

#ifdef DEBUG_CHAN_LOG
  #include <unistd.h>
#endif


ASSUME_NONNULL_BEGIN

// ----------------------------------------------------------------------------
// debugging

#if defined(DEBUG_CHAN_LOG) && !defined(DEBUG)
  #undef DEBUG_CHAN_LOG
#endif
#ifdef DEBUG_CHAN_LOG
  #define THREAD_ID_INVALID  SIZE_MAX

  static usize thread_id() {
    static _Thread_local usize _thread_id = THREAD_ID_INVALID;
    static _Atomic(usize) _thread_id_counter = 0;
    usize tid = _thread_id;
    if (tid == THREAD_ID_INVALID) {
      tid = AtomicAdd(&_thread_id_counter, 1, memory_order_relaxed);
      _thread_id = tid;
    }
    return tid;
  }

  static const char* tcolor() {
    static const char* colors[] = {
      //"\x1b[1m",  // bold (white)
      "\x1b[93m", // yellow
      "\x1b[92m", // green
      "\x1b[91m", // red
      "\x1b[94m", // blue
      "\x1b[96m", // cyan
      "\x1b[95m", // magenta
    };
    return colors[thread_id() % countof(colors)];
  }

  // _dlog_chan writes a log message to stderr along with a globally unique "causality"
  // sequence number. It does not use libc FILEs as those use mutex locks which would alter
  // the behavior of multi-threaded channel operations. Instead it uses a buffer on the stack,
  // which of course is local per thread and then calls the write syscall with the one buffer.
  // This all means that log messages may be out of order; use the "causality" sequence number
  // to understand what other messages were produced according to the CPU.
  ATTR_FORMAT(printf, 2, 3)
  static void _dlog_chan(const char* fname, const char* fmt, ...) {
    static _Atomic(u32) seqnext = 1; // start at 1 to map to line nubmers

    u32 seq = AtomicAdd(&seqnext, 1, memory_order_acquire);

    char buf[256];
    const isize bufcap = (isize)sizeof(buf);
    isize buflen = 0;

    buflen += (isize)snprintf(&buf[buflen], bufcap - buflen,
      "%04u \x1b[1m%sT%02zu ", seq, tcolor(), thread_id());

    va_list ap;
    va_start(ap, fmt);
    buflen += (isize)vsnprintf(&buf[buflen], bufcap - buflen, fmt, ap);
    va_end(ap);

    if (buflen > 0) {
      buflen += (isize)snprintf(&buf[buflen], bufcap - buflen, "\x1b[0m (%s)\n", fname);
      if (buflen >= bufcap) {
        // truncated; make sure to end the line
        buf[buflen - 1] = '\n';
      }
    }

    #undef FMT
    write(STDERR_FILENO, buf, buflen);
  }

  #define dlog_chan(fmt, ...) _dlog_chan(__FUNCTION__, fmt, ##__VA_ARGS__)
  #define dlog_send(fmt, ...) dlog_chan("send: " fmt, ##__VA_ARGS__)
  #define dlog_recv(fmt, ...) dlog_chan("recv: " fmt, ##__VA_ARGS__)
  // #define dlog_send(fmt, ...) do{}while(0)
  // #define dlog_recv(fmt, ...) do{}while(0)
#else
  #define dlog_chan(fmt, ...) do{}while(0)
  #define dlog_send(fmt, ...) do{}while(0)
  #define dlog_recv(fmt, ...) do{}while(0)
#endif


// ----------------------------------------------------------------------------
// misc utils

#define is_power_of_two(intval) \
  (intval) && (0 == ((intval) & ((intval) - 1)))

// is_aligned checks if passed in pointer is aligned on a specific border.
// bool is_aligned<T>(T* pointer, uintptr alignment)
#define is_aligned(pointer, alignment) \
  (0 == ((uintptr)(pointer) & (((uintptr)alignment) - 1)))


// -------------------------------------------------------------------------
// channel lock

#ifdef DEBUG_CHAN_LOCK
  static u32 chlock_count = 0;

  #define chan_lock_t             spinmutex_t
  #define chan_lock_init(lock)    spinmutex_init(lock)
  #define chan_lock_dispose(lock) spinmutex_dispose(lock)

  #define chan_lock(lock) do{ \
    u32 n = chlock_count++; \
    dlog("CL #%u LOCK %s:%d", n, __FILE__, __LINE__); \
    spinmutex_lock(lock); \
    dlog("CL #%u UNLOCK %s:%d", n, __FILE__, __LINE__); \
  }while(0)

  #define chan_unlock(lock) do{ \
    /*dlog("CL UNLOCK %s:%d", __FILE__, __LINE__);*/ \
    spinmutex_unlock(lock); \
  }while(0)
#else
  #define chan_lock_t             spinmutex_t
  #define chan_lock_init(lock)    spinmutex_init(lock)
  #define chan_lock_dispose(lock) spinmutex_dispose(lock)
  #define chan_lock(lock)         spinmutex_lock(lock)
  #define chan_unlock(lock)       spinmutex_unlock(lock)
#endif

// semaphore implementation
#if 0
  // use high-contention semaphore, which spins the CPU for a while before
  // sleeping the thread
  #define chan_sema_t            hcsema_t
  #define chan_sema_init         hcsema_init
  #define chan_sema_signal(sema) hcsema_signal((sema), 1)
  #define chan_sema_wait         hcsema_wait
#else
  // use the regular host-platform semaphore
  #define chan_sema_t            sema_t
  #define chan_sema_init         sema_init
  #define chan_sema_signal(sema) sema_signal((sema), 1)
  #define chan_sema_wait         sema_wait
#endif

// -------------------------------------------------------------------------

typedef struct Thr Thr;


// Thr holds thread-specific data and is owned by thread-local storage
struct Thr {
  usize          id;
  bool           init;
  _Atomic(bool)  closed;
  chan_sema_t    sema;
  Thr* nullable  next ATTR_ALIGNED_CACHE_LINE; // list link
  _Atomic(void*) elemptr;
};

typedef struct WaitQ {
  _Atomic(Thr*) first; // head of linked list of parked threads
  _Atomic(Thr*) last;  // tail of linked list of parked threads
} WaitQ;

typedef struct chan_t {
  // These fields don't change after chan_open
  memalloc_t ma;      // memory allocator this belongs to (immutable)
  usize      elemsize; // size in bytes of elements sent on the channel
  u32        qcap;     // size of the circular queue buf (immutable)
  #if !defined(USE_ALIGNED_ALLOC)
  u32        memoffs;  // offset from chan_t memory address of actual allocation
  #endif

  // These fields are frequently accessed and stored to.
  // There's a perf opportunity here with a different more cache-efficient layout.
  _Atomic(u32)  qlen;   // number of messages currently queued in buf
  _Atomic(bool) closed; // one way switch (once it becomes true, never becomes false again)
  chan_lock_t   lock;   // guards the chan_t struct

  // sendq is accessed on every call to _chan_recv and only in some cases by _chan_send,
  // when parking a thread when there's no waiting receiver nor queued message.
  // recvq is accessed on every call to _chan_send and like sendq, only when parking a thread
  // in _chan_recv.
  WaitQ sendq; // list of waiting send callers
  WaitQ recvq; // list of waiting recv callers

  // sendx & recvx are likely to be falsely shared between threads.
  // - sendx is loaded & stored by both _chan_send and _chan_recv
  //   - _chan_send for buffered channels when no receiver is waiting
  //   - _chan_recv when there's a waiting sender
  // - recvx is only used by _chan_recv
  // So we make sure recvx ends up on a separate cache line.
  _Atomic(u32) sendx; // send index in buf
  _Atomic(u32) recvx ATTR_ALIGNED_CACHE_LINE; // receive index in buf

  // u8 pad[CACHE_LINE_SIZE];
  u8 buf[]; // queue storage
} ATTR_ALIGNED_CACHE_LINE chan_t;


static void thr_init(Thr* t) {
  static _Atomic(usize) _thread_id_counter = 0;

  t->id = AtomicAdd(&_thread_id_counter, 1, memory_order_relaxed);
  t->init = true;
  safecheckx(chan_sema_init(&t->sema, 0) == 0/*no error*/); // TODO: SemaDispose?
}


inline static Thr* thr_current() {
  static _Thread_local Thr _thr = {0};

  Thr* t = &_thr;
  if (UNLIKELY(!t->init))
    thr_init(t);
  return t;
}


inline static void thr_signal(Thr* t) {
  chan_sema_signal(&t->sema); // wake
}


inline static void thr_wait(Thr* t) {
  dlog_chan("thr_wait ...");
  chan_sema_wait(&t->sema); // sleep
}


static void wq_enqueue(WaitQ* wq, Thr* t) {
  // note: atomic loads & stores for cache reasons, not thread safety; c->lock is held.
  if (AtomicLoadAcq(&wq->first)) {
    // Note: compare first instead of last as we don't clear wq->last in wq_dequeue
    AtomicLoadAcq(&wq->last)->next = t;
  } else {
    AtomicStoreRel(&wq->first, t);
  }
  AtomicStoreRel(&wq->last, t);
}


inline static Thr* nullable wq_dequeue(WaitQ* wq) {
  Thr* t = AtomicLoadAcq(&wq->first);
  if (t) {
    AtomicStoreRel(&wq->first, t->next);
    t->next = NULL;
    // Note: intentionally not clearing wq->last in case wq->first==wq->last as we can
    // avoid that branch by not checking wq->last in wq_enqueue.
  }
  return t;
}


// chan_bufptr returns the pointer to the i'th slot in the buffer
inline static void* chan_bufptr(chan_t* c, u32 i) {
  return (void*)&c->buf[(uintptr)i * (uintptr)c->elemsize];
}


// chan_park adds elemptr to wait queue wq, unlocks channel c and blocks the calling thread
static Thr* chan_park(chan_t* c, WaitQ* wq, void* elemptr) {
  // caller must hold lock on channel that owns wq
  Thr* t = thr_current();
  AtomicStore(&t->elemptr, elemptr, memory_order_relaxed);
  dlog_chan("park: elemptr %p", elemptr);
  wq_enqueue(wq, t);
  chan_unlock(&c->lock);
  thr_wait(t);
  return t;
}


inline static bool chan_full(chan_t* c) {
  // c.qcap is immutable (never written after the channel is created)
  // so it is safe to read at any time during channel operation.
  if (c->qcap == 0)
    return AtomicLoad(&c->recvq.first, memory_order_relaxed) == NULL;
  return AtomicLoad(&c->qlen, memory_order_relaxed) == c->qcap;
}


static bool _chan_send_direct(chan_t* c, void* srcelemptr, Thr* recvt) {
  // _chan_send_direct processes a send operation on an empty channel c.
  // element sent by the sender is copied to the receiver recvt.
  // The receiver is then woken up to go on its merry way.
  // Channel c must be empty and locked. This function unlocks c with chan_unlock.
  // recvt must already be dequeued from c.

  void* dstelemptr = AtomicLoadAcq(&recvt->elemptr);
  assertnotnull(dstelemptr);
  dlog_send("direct send of srcelemptr %p to [%zu] (dstelemptr %p)",
    srcelemptr, recvt->id, dstelemptr);
  // store to address provided with chan_recv call
  memcpy(dstelemptr, srcelemptr, c->elemsize);
  // clear pointer (TODO: is this really needed?)
  AtomicStore(&recvt->elemptr, NULL, memory_order_relaxed);

  chan_unlock(&c->lock);
  thr_signal(recvt); // wake up chan_recv caller
  return true;
}


inline static bool _chan_send(chan_t* c, void* srcelemptr, bool* nullable closed) {
  bool block = closed == NULL;
  dlog_send("srcelemptr %p", srcelemptr);

  // fast path for non-blocking send on full channel
  //
  // From Go's chan implementation from which this logic is borrowed:
  // After observing that the channel is not closed, we observe that the channel is
  // not ready for sending. Each of these observations is a single word-sized read
  // (first c.closed and second chan_full()).
  // Because a closed channel cannot transition from 'ready for sending' to
  // 'not ready for sending', even if the channel is closed between the two observations,
  // they imply a moment between the two when the channel was both not yet closed
  // and not ready for sending. We behave as if we observed the channel at that moment,
  // and report that the send cannot proceed.
  //
  // It is okay if the reads are reordered here: if we observe that the channel is not
  // ready for sending and then observe that it is not closed, that implies that the
  // channel wasn't closed during the first observation. However, nothing here
  // guarantees forward progress. We rely on the side effects of lock release in
  // chan_recv() and ChanClose() to update this thread's view of c.closed and chan_full().
  if (!block && !c->closed && chan_full(c))
    return false;

  chan_lock(&c->lock);

  if (UNLIKELY(AtomicLoad(&c->closed, memory_order_relaxed))) {
    chan_unlock(&c->lock);
    if (block) {
      panic("send on closed channel");
    } else {
      *closed = true;
    }
    return false;
  }

  Thr* recvt = wq_dequeue(&c->recvq);
  if (recvt) {
    // Found a waiting receiver. recvt is blocked, waiting in chan_recv.
    // We pass the value we want to send directly to the receiver,
    // bypassing the channel buffer (if any).
    // Note that _chan_send_direct calls chan_unlock(&c->lock).
    assert(recvt->init);
    return _chan_send_direct(c, srcelemptr, recvt);
  }

  if (AtomicLoad(&c->qlen, memory_order_relaxed) < c->qcap) {
    // space available in message buffer -- enqueue
    u32 i = AtomicAdd(&c->sendx, 1, memory_order_relaxed);
    // copy *srcelemptr -> *dstelemptr
    void* dstelemptr = chan_bufptr(c, i);
    memcpy(dstelemptr, srcelemptr, c->elemsize);
    dlog_send("enqueue elemptr %p at buf[%u]", srcelemptr, i);
    if (i == c->qcap - 1)
      AtomicStore(&c->sendx, 0, memory_order_relaxed);
    AtomicAdd(&c->qlen, 1, memory_order_relaxed);
    chan_unlock(&c->lock);
    return true;
  }

  // buffer is full and there is no waiting receiver
  if (!block) {
    chan_unlock(&c->lock);
    return false;
  }

  // park the calling thread. Some recv caller will wake us up.
  // Note that chan_park calls chan_unlock(&c->lock)
  dlog_send("wait... (elemptr %p)", srcelemptr);
  chan_park(c, &c->sendq, srcelemptr);
  dlog_send("woke up -- sent elemptr %p", srcelemptr);
  return true;
}


// chan_empty reports whether a read from c would block (that is, the channel is empty).
// It uses a single atomic read of mutable state.
inline static bool chan_empty(chan_t* c) {
  // Note: qcap is immutable
  if (c->qcap == 0)
    return AtomicLoad(&c->sendq.first, memory_order_relaxed) == NULL;
  return AtomicLoad(&c->qlen, memory_order_relaxed) == 0;
}


static bool _chan_recv_direct(chan_t* c, void* dstelemptr, Thr* st);


inline static bool _chan_recv(chan_t* c, void* dstelemptr, bool* nullable closed) {
  bool block = closed == NULL; // TODO: non-blocking path
  dlog_recv("dstelemptr %p", dstelemptr);

  // Fast path: check for failed non-blocking operation without acquiring the lock.
  if (!block && chan_empty(c)) {
    // After observing that the channel is not ready for receiving, we observe whether the
    // channel is closed.
    //
    // Reordering of these checks could lead to incorrect behavior when racing with a close.
    // For example, if the channel was open and not empty, was closed, and then drained,
    // reordered reads could incorrectly indicate "open and empty". To prevent reordering,
    // we use atomic loads for both checks, and rely on emptying and closing to happen in
    // separate critical sections under the same lock.  This assumption fails when closing
    // an unbuffered channel with a blocked send, but that is an error condition anyway.
    if (AtomicLoad(&c->closed, memory_order_relaxed) == false) {
      // Because a channel cannot be reopened, the later observation of the channel
      // being not closed implies that it was also not closed at the moment of the
      // first observation. We behave as if we observed the channel at that moment
      // and report that the receive cannot proceed.
      return false;
    }
    // The channel is irreversibly closed. Re-check whether the channel has any pending data
    // to receive, which could have arrived between the empty and closed checks above.
    // Sequential consistency is also required here, when racing with such a send.
    if (chan_empty(c)) {
      // The channel is irreversibly closed and empty
      memset(dstelemptr, 0, c->elemsize);
      *closed = true;
      return false;
    }
  }

  chan_lock(&c->lock);

  if (AtomicLoad(&c->closed, memory_order_relaxed) &&
      AtomicLoad(&c->qlen, memory_order_relaxed) == 0)
  {
    // channel is closed and the buffer queue is empty
    dlog_recv("channel closed & empty queue");
    chan_unlock(&c->lock);
    memset(dstelemptr, 0, c->elemsize);
    if (closed)
      *closed = true;
    return false;
  }

  Thr* t = wq_dequeue(&c->sendq);
  if (t) {
    // Found a waiting sender.
    // If buffer is size 0, receive value directly from sender.
    // Otherwise, receive from head of queue and add sender's value to the tail of the queue
    // (both map to the same buffer slot because the queue is full).
    // Note that _chan_recv_direct calls chan_unlock(&c->lock).
    assert(t->init);
    return _chan_recv_direct(c, dstelemptr, t);
  }

  if (AtomicLoad(&c->qlen, memory_order_relaxed) > 0) {
    // Receive directly from queue
    u32 i = AtomicAdd(&c->recvx, 1, memory_order_relaxed);
    if (i == c->qcap - 1)
      AtomicStore(&c->recvx, 0, memory_order_relaxed);
    AtomicSub(&c->qlen, 1, memory_order_relaxed);

    // copy *srcelemptr -> *dstelemptr
    void* srcelemptr = chan_bufptr(c, i);
    memcpy(dstelemptr, srcelemptr, c->elemsize);
    #ifdef DEBUG
    memset(srcelemptr, 0, c->elemsize); // zero buffer memory
    #endif

    dlog_recv("dequeue elemptr %p from buf[%u]", srcelemptr, i);

    chan_unlock(&c->lock);
    return true;
  }

  // No message available -- nothing queued and no waiting senders
  if (!block) {
    chan_unlock(&c->lock);
    return false;
  }

  // Check if the channel is closed.
  if (AtomicLoad(&c->closed, memory_order_relaxed)) {
    chan_unlock(&c->lock);
    goto ret_closed;
  }

  // Block by parking the thread. Some send caller will wake us up.
  // Note that chan_park calls chan_unlock(&c->lock)
  dlog_recv("wait... (elemptr %p)", dstelemptr);
  t = chan_park(c, &c->recvq, dstelemptr);

  // woken up by sender or close call
  if (AtomicLoad(&t->closed, memory_order_relaxed)) {
    // Note that we check "closed" on the Thr, not the Chan.
    // This is important since c->closed may be true even as we receive a message.
    dlog_recv("woke up -- channel closed");
    goto ret_closed;
  }

  // message was delivered by storing to elemptr by some sender
  dlog_recv("woke up -- received to elemptr %p", dstelemptr);
  return true;

ret_closed:
  dlog_recv("channel closed");
  memset(dstelemptr, 0, c->elemsize);
  return false;
}


// _chan_recv_direct processes a receive operation on a full channel c
static bool _chan_recv_direct(chan_t* c, void* dstelemptr, Thr* sendert) {
  // There are 2 parts:
  // 1) The value sent by the sender sg is put into the channel and the sender
  //    is woken up to go on its merry way.
  // 2) The value received by the receiver (the current G) is written to ep.
  // For synchronous (unbuffered) channels, both values are the same.
  // For asynchronous (buffered) channels, the receiver gets its data from
  // the channel buffer and the sender's data is put in the channel buffer.
  // Channel c must be full and locked.
  // sendert must already be dequeued from c.sendq.
  bool ok = true;

  if (AtomicLoad(&c->qlen, memory_order_relaxed) == 0) {
    // Copy data from sender
    void* srcelemptr = AtomicLoad(&sendert->elemptr, memory_order_consume);
    dlog_recv("direct recv of srcelemptr %p from [%zu] (dstelemptr %p, buffer empty)",
      srcelemptr, sendert->id, dstelemptr);
    assertnotnull(srcelemptr);
    memcpy(dstelemptr, srcelemptr, c->elemsize);
  } else {
    // Queue is full. Take the item at the head of the queue.
    // Make the sender enqueue its item at the tail of the queue.
    // Since the queue is full, those are both the same slot.
    dlog_recv("direct recv from [%zu] (dstelemptr %p, buffer full)", sendert->id, dstelemptr);
    //assert_debug(AtomicLoad(&c->qlen, memory_order_relaxed) == c->qcap); // queue full

    // copy element from queue to receiver
    u32 i = AtomicAdd(&c->recvx, 1, memory_order_relaxed);
    if (i == c->qcap - 1) {
      AtomicStore(&c->recvx, 0, memory_order_relaxed);
      AtomicStore(&c->sendx, 0, memory_order_relaxed);
    } else {
      AtomicStore(&c->sendx, i + 1, memory_order_relaxed);
    }

    // copy c->buf[i] -> *dstelemptr
    void* bufelemptr = chan_bufptr(c, i);
    assertnotnull(bufelemptr);
    memcpy(dstelemptr, bufelemptr, c->elemsize);
    dlog_recv("dequeue srcelemptr %p from buf[%u]", bufelemptr, i);

    // copy *sendert->elemptr -> c->buf[i]
    void* srcelemptr = AtomicLoad(&sendert->elemptr, memory_order_consume);
    assertnotnull(srcelemptr);
    memcpy(bufelemptr, srcelemptr, c->elemsize);
    dlog_recv("enqueue srcelemptr %p to buf[%u]", srcelemptr, i);
  }

  chan_unlock(&c->lock);
  thr_signal(sendert); // wake up _chan_send caller
  return ok;
}


static usize chan_memsize_align(usize memsize) {
  #ifdef USE_ALIGNED_ALLOC
    return ALIGN2(memsize, CACHE_LINE_SIZE);
  #else
    return ALIGN2(memsize + ((CACHE_LINE_SIZE+1) / 2), CACHE_LINE_SIZE);
  #endif
}


static usize chan_memsize_checked(usize elemsize, usize bufcap) {
  usize memsize;
  if (check_mul_overflow(elemsize, bufcap, &memsize))
    return USIZE_MAX;
  if (check_add_overflow(memsize, sizeof(chan_t), &memsize))
    return USIZE_MAX;
  if UNLIKELY(memsize > USIZE_MAX - CACHE_LINE_SIZE)
    return USIZE_MAX;
  return chan_memsize_align(memsize);
}


// used to free the chan
static usize chan_memsize_unchecked(usize elemsize, usize bufcap) {
  usize memsize = sizeof(chan_t) + (bufcap * elemsize);
  return chan_memsize_align(memsize);
}


chan_t* chan_open(memalloc_t ma, usize elemsize, u32 bufcap) {
  usize memsize = chan_memsize_checked(elemsize, (usize)bufcap);
  if UNLIKELY(memsize == USIZE_MAX) {
    dlog("overflow");
    return NULL;
  }

  #ifdef USE_ALIGNED_ALLOC
    chan_t* c = mem_alloc(ma, memsize).p;
    if UNLIKELY(c == NULL) {
      dlog("out of memory");
      return NULL;
    }
    memset(c, 0, sizeof(chan_t));
  #else
    mem_t m = mem_alloc(ma, memsize);
    if UNLIKELY(m.p == NULL) {
      dlog("out of memory");
      return NULL;
    }
    // align c to line cache boundary
    chan_t* c = (chan_t*)ALIGN2((uintptr)m.p, CACHE_LINE_SIZE);
    memset(c, 0, sizeof(chan_t));
    c->memoffs = (u32)(uintptr)((void*)c - m.p);
  #endif

  c->ma = ma;
  c->elemsize = elemsize;
  c->qcap = bufcap;
  safecheckx(chan_lock_init(&c->lock) == 0 /*no error*/);

  // make sure that the thread setting up the channel gets a low thread_id
  #ifdef DEBUG_CHAN_LOG
  thread_id();
  #endif

  return c;
}


void chan_close(chan_t* c) {
  dlog_chan("--- close ---");

  chan_lock(&c->lock);
  dlog_chan("close: channel locked");

  if (atomic_exchange_explicit(&c->closed, 1, memory_order_acquire) != 0)
    panic("close of closed channel");
  atomic_thread_fence(memory_order_seq_cst);

  Thr* t = AtomicLoadAcq(&c->recvq.first);
  while (t) {
    dlog_chan("close: wake recv [%zu]", t->id);
    Thr* next = t->next;
    AtomicStore(&t->closed, true, memory_order_relaxed);
    thr_signal(t);
    t = next;
  }

  t = AtomicLoadAcq(&c->sendq.first);
  while (t) {
    dlog_chan("close: wake send [%zu]", t->id);
    Thr* next = t->next;
    AtomicStore(&t->closed, true, memory_order_relaxed);
    thr_signal(t);
    t = next;
  }

  chan_unlock(&c->lock);
  dlog_chan("close: done");
}


void chan_free(chan_t* c) {
  assert(AtomicLoadAcq(&c->closed)); // must close channel before freeing its memory
  chan_lock_dispose(&c->lock);
  usize memsize = chan_memsize_unchecked(c->elemsize, c->qcap);
  #ifdef USE_ALIGNED_ALLOC
    mem_freex(c->ma, MEM(c, memsize));
  #else
    void* p = (void*)((uintptr)c - (uintptr)c->memoffs);
    mem_freex(c->ma, MEM(p, memsize));
  #endif
}


u32 chan_cap(const chan_t* c) {
  return c->qcap;
}

bool chan_send(chan_t* c, void* elemptr) {
  return _chan_send(c, elemptr, NULL);
}

bool chan_recv(chan_t* c, void* elemptr) {
  return _chan_recv(c, elemptr, NULL);
}

bool chan_trysend(chan_t* c, void* elemptr, bool* closed) {
  return _chan_send(c, elemptr, closed);
}

bool chan_tryrecv(chan_t* c, void* elemptr, bool* closed) {
  return _chan_recv(c, elemptr, closed);
}


ASSUME_NONNULL_END
