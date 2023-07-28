// thread safe bump allocator backed by vm pages
// SPDX-License-Identifier: Apache-2.0
//
// Currently it sources vm pages as it grows and then when it's disposed all vm pages
// are returned to the OS. This could probably be made more efficient by using a
// page manager (like "memory manager" in RSM) to centrally pool all pages for the
// process, handing out and taking back pages for allocators.
//
#include "colib.h"
#include "thread.h"


// MIN_ALIGNMENT: minimum alignment for allocations
#define MIN_ALIGNMENT sizeof(void*)


// DEFAULT_SLABSIZE: minimum size to allocate for new slabs
#define DEFAULT_SLABSIZE (1024ul * 1024ul)


// ALWAYS_ISZERO is defined if the target we are building for always and
// unconditionally returns zeroed pages from mmap(MAP_ANONYMOUS).
//
// bool ISZERO(const bump_allocator_t* a)
// Returns true if the OS returns zeroed pages from mmap(MAP_ANONYMOUS)
//
#if defined(__linux__) || \
    defined(__FreeBSD__) || defined(__NetBSD__) || defined(__OpenBSD__) || \
    defined(__hpux)
  // https://man7.org/linux/man-pages/man2/mmap.2.html
  // https://nixdoc.net/man-pages/HP-UX/man2/mmap.2.html
  // https://man.netbsd.org/mmap.2
  #define ALWAYS_ISZERO
  #define ISZERO(a) 1
#else
  // assume "no"
  #define ISZERO(a) 0
#endif


// slab_t is the header of each region of vm pages allocated by bump_allocator_t
typedef struct slab_ slab_t;
typedef struct slab_ {
  usize   size;
  slab_t* prev;
} slab_t;

// bump_alloc_grow assumes that sizeof(slab_t) is an even multiple of MIN_ALIGNMENT
static_assert(IS_ALIGN2(sizeof(slab_t), MIN_ALIGNMENT), "");

// bump_allocator_t contains book-keeping data for the allocator
typedef struct {
  slab_t           head;   // caution: cyclic; head->prev initially points to &head
  _Atomic(slab_t*) tail;   // caution: cyclic; tail initially points to &head
  mutex_t          tailmu; // guards modifications to tail
  _Atomic(void*)   end;    // end of backing memory (== tail + tail->size)
  _Atomic(void*)   ptr;    // next allocation (>= tail)
  struct memalloc  ma;
} bump_allocator_t;


// bump_allocator_t* BUMPALLOC_OF_MEMALLOC(memalloc_t ma)
// Casts a user memalloc_t to a bump_allocator_t
#define BUMPALLOC_OF_MEMALLOC(ma_) \
  ((bump_allocator_t*)( ((void*)(ma_)) - offsetof(bump_allocator_t, ma) ))


// ATOMIC_{STORE,LOAD} convenience macros
#define ATOMIC_STORE(p, v) AtomicStore((p), (v), memory_order_release)
#define ATOMIC_LOAD(p)     AtomicLoad((p), memory_order_acquire)


static bool bump_alloc_grow(bump_allocator_t* a, usize size) {
  assert(IS_ALIGN2(size, MIN_ALIGNMENT)); // bump_alloc has aligned it

  mutex_lock(&a->tailmu);

  // load current tail
  slab_t* oldtail = ATOMIC_LOAD(&a->tail);

  // include space for the slab header
  size += sizeof(slab_t);

  // allocate at least a->head.size/sys_pagesize() pages
  size = MAX(size, a->head.size);

  // calculate ideal address for new pages, just after our current range
  void* at_addr = (void*)oldtail + oldtail->size;

  mem_t m = sys_vm_alloc(at_addr, size);

  if UNLIKELY(m.p == NULL) {
    dlog("%s: sys_vm_alloc(%p, %zu) failed", __FUNCTION__, at_addr, size);
    mutex_unlock(&a->tailmu);
    return false;
  }

  // dlog("%s: alloc(%p, %zu) %p .. %p (%zu)",
  //   __FUNCTION__, at_addr, size, m.p, m.p+m.size, m.size);

  slab_t* slab = m.p;
  slab->size = m.size;
  slab->prev = assertnotnull(oldtail);

  ATOMIC_STORE(&a->tail, slab);

  void* end = (void*)slab + slab->size;
  void* ptr = (void*)ALIGN2((uintptr)slab + sizeof(slab_t), MIN_ALIGNMENT);

  assert(IS_ALIGN2((uintptr)ptr, MIN_ALIGNMENT));
  assert(ptr >= (void*)slab + sizeof(slab_t));

  ATOMIC_STORE(&a->end, end);
  ATOMIC_STORE(&a->ptr, ptr);

  assertnotnull(ATOMIC_LOAD(&a->tail)->prev);
  // dlog("stored tail %p, prev %p", slab, slab->prev);

  mutex_unlock(&a->tailmu);

  return true;
}


static bool bump_alloc(bump_allocator_t* a, mem_t* m, usize size, bool zeroed) {
  void* oldptr;
  void* newptr;

  size = ALIGN2(size, MIN_ALIGNMENT);
  oldptr = ATOMIC_LOAD(&a->ptr);

  for (;;) {
    // allocate another slab if needed
    if UNLIKELY(oldptr + size > ATOMIC_LOAD(&a->end)) {
      if (!bump_alloc_grow(a, size)) {
        *m = (mem_t){0};
        return false;
      }
      oldptr = ATOMIC_LOAD(&a->ptr);
      // must loop & check oldptr+size>a->end again
    } else {
      newptr = oldptr + size;
      if LIKELY(AtomicCASAcqRel(&a->ptr, &oldptr, newptr)) {
        m->p = oldptr;
        m->size = size;
        a->ptr = newptr;
        if (zeroed && !ISZERO(a))
          memset(m->p, 0, size);
        return true;
      }
      // another thread raced us and won
      // AtomicCAS has updated oldptr to the current value
    }
  }
  UNREACHABLE;
  return false;
}


static bool bump_resize(bump_allocator_t* a, mem_t* m, usize size, bool zeroed) {
  // TODO: grow tail if we can (i.e. when m.p+m.size==a->ptr-m.size)
  mem_t newmem = {0};
  if (!bump_alloc(a, &newmem, size, zeroed))
    return false;
  memcpy(newmem.p, m->p, m->size);
  if (zeroed && !ISZERO(a))
    memset(newmem.p + m->size, 0, newmem.size - m->size);
  m->p = newmem.p;
  m->size = newmem.size;
  return true;
}


static bool bump_free(bump_allocator_t* a, mem_t* m, usize size, bool zeroed) {
  // attempt to free tail when m.p+m.size==a->ptr
  void* oldptr = ATOMIC_LOAD(&a->ptr);
  if UNLIKELY(oldptr == m->p + m->size) {
    // zero the returned memory (if free memory is assumed to be zero)
    if (ISZERO(a))
      memset(m->p, 0, m->size);
    // attempt to decrement a->ptr
    void* newptr = oldptr - m->size;
    AtomicCASAcqRel(&a->ptr, &oldptr, newptr);
    // If another thread "won" and we didn't decrement ptr,
    // then there's nothing else to do (we just leave the allocation.)
    //
    // Note: we must never "free" tail (ie a->tail=a->tail->prev) since that would
    // break monotonicity which is assumed to make the impl thread safe.
  }
  *m = (mem_t){0};
  return true;
}


static bool _memalloc_bump_impl(void* ma, mem_t* m, usize size, bool zeroed) {
  bump_allocator_t* a = BUMPALLOC_OF_MEMALLOC(ma);
  assertnotnull(m);
  if (m->p == NULL)
    return bump_alloc(a, m, size, zeroed);
  if (size != 0)
    return bump_resize(a, m, size, zeroed);
  return bump_free(a, m, size, zeroed);
}


memalloc_t memalloc_bump2(usize slabsize, u32 flags) {
  assert(flags == 0); // no flags, for now

  // adjust slabsize
  usize pagesize = sys_pagesize();
  if (slabsize == 0)
    slabsize = DEFAULT_SLABSIZE;
  slabsize = ALIGN2(slabsize, pagesize);

  // map initial vm pages
  mem_t m = sys_vm_alloc(NULL, slabsize);
  if (!m.p) {
    dlog("%s: sys_vm_alloc(%zu) failed", __FUNCTION__, slabsize);
    return &_memalloc_null;
  }
  safecheckf(m.size > sizeof(bump_allocator_t),
    "requested %zu, got %zu", slabsize, m.size);

  // the allocator's book-keeping data lives at the beginning of the first page
  bump_allocator_t* a = m.p;
  a->head.size = m.size;
  a->head.prev = &a->head;
  a->tail = &a->head;
  err_t err = mutex_init(&a->tailmu);
  a->end = m.p + m.size;
  a->ptr = (void*)ALIGN2((uintptr)a->tail + sizeof(bump_allocator_t), MIN_ALIGNMENT);
  a->ma.f = _memalloc_bump_impl;

  if UNLIKELY(err) {
    dlog("%s: mutex_init failed (%s)", __FUNCTION__, err_str(err));
    memalloc_bump2_dispose(&a->ma);
    return &_memalloc_null;
  }

  return &a->ma;
}


void memalloc_bump2_dispose(memalloc_t ma) {
  if (ma == &_memalloc_null)
    return;
  assert(ma->f == _memalloc_bump_impl);
  bump_allocator_t* a = BUMPALLOC_OF_MEMALLOC(ma);

  mutex_dispose(&a->tailmu);

  err_t err;
  slab_t* slab = a->tail;
  slab_t* head = &a->head;
  slab_t* prev_slab;
  for (;;) {
    prev_slab = slab->prev;
    if (( err = sys_vm_free(MEM(slab, slab->size)) ))
      dlog("%s: sys_vm_free failed: %s", __FUNCTION__, err_str(err));
    if (slab == head)
      break;
    slab = prev_slab;
  }
}


usize memalloc_bump2_cap(memalloc_t ma) {
  const bump_allocator_t* a = BUMPALLOC_OF_MEMALLOC(ma);
  const slab_t* slab = ATOMIC_LOAD(&a->tail);
  usize cap = 0;
  for (;;) {
    cap += slab->size;
    if (slab == &a->head)
      break;
    slab = slab->prev;
  }
  return cap - sizeof(bump_allocator_t);
}


usize memalloc_bump2_use(memalloc_t ma) {
  bump_allocator_t* a = BUMPALLOC_OF_MEMALLOC(ma);

  const void* ptr;
  const slab_t* tail;

  for (;;) {
    // data race when loading ptr & tail
    tail = ATOMIC_LOAD(&a->tail);
    ptr = ATOMIC_LOAD(&a->ptr);
    if (ptr <= (void*)tail + tail->size && (void*)tail < ptr)
      break;
  }

  // use of current tail slab
  usize use = (usize)(uintptr)(ptr - (uintptr)tail);

  // count all preceding slabs as being fully in use
  if (tail != &a->head) {
    const slab_t* slab = tail->prev;
    for (;;) {
      use += slab->size;
      if (slab == &a->head)
        break;
      slab = slab->prev;
    }
  }

  return use - sizeof(bump_allocator_t);
}


usize memalloc_bump2_avail(memalloc_t ma) {
  bump_allocator_t* a = BUMPALLOC_OF_MEMALLOC(ma);

  const void* ptr;
  const void* end;

  // data race when loading ptr & end
  for (;;) {
    end = ATOMIC_LOAD(&a->end);
    ptr = ATOMIC_LOAD(&a->ptr);
    if (ptr <= end)
      break;
  }

  return (usize)(uintptr)(end - ptr);
}


//———————————————————————————————————————————————————————————————————————————————————————
// tests
#ifdef CO_ENABLE_TESTS

#include "hash.h"
#include <unistd.h> // write


// unlike most printf implementations, tlog does not cause thread syncing
#define tlog(fmt, args...) \
  _tlog("tlog> " fmt " (%s:%d)", ##args, __FILE__, __LINE__)
ATTR_FORMAT(printf, 1, 2) UNUSED static void _tlog(const char* fmt, ...) {
  char buf[4096];
  va_list ap;
  va_start(ap, fmt);
  usize n = vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  if (n >= sizeof(buf))
    n = sizeof(buf) - 1;
  buf[n] = '\n';
  write(1, buf, n + 1);
}


__attribute__((constructor)) static void test_memalloc_bump2() {
  u32 flags = 0;
  usize slabsize = 1; // make slabs as small as possible
  memalloc_t ma = memalloc_bump2(slabsize, flags);
  bump_allocator_t* a = BUMPALLOC_OF_MEMALLOC(ma);

  //dlog("effective slabsize: %zu", a->head.size);

  assert(memalloc_bump2_use(ma) == 0);
  assert(memalloc_bump2_cap(ma) == a->head.size - sizeof(bump_allocator_t));
  assert(memalloc_bump2_avail(ma) == a->head.size - sizeof(bump_allocator_t));

  mem_t m;
  usize avail = memalloc_bump2_avail(ma);

  // allocate almost everything
  m = mem_alloc(ma, avail - MIN_ALIGNMENT);
  assert(m.size == avail - MIN_ALIGNMENT);

  // allocate last remaining memory
  m = mem_alloc(ma, MIN_ALIGNMENT);
  assert(m.size == MIN_ALIGNMENT);

  // should now have zero available space
  assert(memalloc_bump2_avail(ma) == 0);

  // freeing the tail should be effective
  mem_free(ma, &m);
  m = mem_alloc(ma, MIN_ALIGNMENT);
  assert(m.size == MIN_ALIGNMENT);

  // prev call should not have caused growth
  assertf(memalloc_bump2_avail(ma) == 0, "mem_free didn't work as expected");

  // allocate minimum size should cause allocator to attempt to grow,
  // requesting more vm pages from the OS
  m = mem_alloc(ma, MIN_ALIGNMENT);
  assertf(m.size == MIN_ALIGNMENT, "failed to grow vm allocation");

  // allocate remaining space
  //dlog("avail: %zu", memalloc_bump2_avail(ma));
  avail = memalloc_bump2_avail(ma);
  m = mem_alloc(ma, avail);
  assert(m.size == avail);
  assertf(memalloc_bump2_avail(ma) == 0, "should have no free space");

  // Now, try to allocate a vm page just after the allocation.
  // This should cause bump_alloc_grow to allocate a non-contiguous slab
  mem_t page;
  {
    void* at_addr = (void*)a->tail + a->tail->size;
    page = sys_vm_alloc(at_addr, 1);
    assertf(page.p != NULL, "sys_vm_alloc_at(%p) failed", at_addr);
    //dlog("allocated vm page %p (%p)", page.p, at_addr);
  }

  // allocate minimum size should cause allocator to attempt to grow
  m = mem_alloc(ma, MIN_ALIGNMENT);
  assertf(m.size == MIN_ALIGNMENT, "failed to grow vm allocation");

  sys_vm_free(page);
  memalloc_bump2_dispose(ma);
  log("%s: PASSED", __FUNCTION__);
}


typedef struct {
  thrd_t         t;  // thread handle
  memalloc_t     ma; // shared bump2 allocator
  _Atomic(mem_t) m1; // sample allocation
  sema_t*        sem_ready;
  sema_t*        sem_go;
} test_thread_t;


static int test_thread(test_thread_t* t) {
  mem_t m;
  usize avail;
  memalloc_t ma = t->ma;

  // tlog("test_thread %p ready", t->t);
  assert(sema_signal(t->sem_ready, 1));
  assert(sema_wait(t->sem_go));

  // maximum allocation; cause race on bump_alloc_grow
  avail = memalloc_bump2_avail(ma);
  m = mem_alloc_zeroed(ma, avail + 1);
  assert(m.size >= avail + 1);

  // check that we actually got zeroed memory
  static_assert(MIN_ALIGNMENT >= sizeof(uintptr), "");
  assert(IS_ALIGN2(m.size, sizeof(uintptr)));
  for (uintptr* p = m.p; p < (uintptr*)(m.p + m.size); p++)
    assert(*p == 0);

  // save allocation for later inspection
  ATOMIC_STORE(&t->m1, m);

  return 0;
}


__attribute__((constructor)) static void test_memalloc_bump2_mt() {
  u32 threadc = sys_ncpu();
  if (threadc == 1) {
    log("%s: SKIP (only 1 CPU)", __FUNCTION__);
    return;
  }

  // spawn 4x as many threads as there are logical CPUs
  threadc *= 4;

  // allocate memory for thread structs
  test_thread_t* threadv = mem_alloctv(
    memalloc_default(), test_thread_t, threadc);
  assertnotnull(threadv);

  // create a bump2 allocator
  u32 flags = 0;
  memalloc_t ma = memalloc_bump2(/*slabsize*/0, flags);

  // create sync semaphores.
  // this approach maximizes our chances for races
  sema_t sem_ready;
  sema_t sem_go;
  assert(sema_init(&sem_ready, 0) == 0);
  assert(sema_init(&sem_go, 0) == 0);

  // spawn threads
  //tlog("spawning %u 'test_thread_t's", threadc);
  for (u32 i = 0; i < threadc; i++) {
    test_thread_t* t = &threadv[i];
    t->ma = ma;
    t->sem_ready = &sem_ready;
    t->sem_go = &sem_go;
    int err = thrd_create(&t->t, (thrd_start_t)test_thread, t);
    assert(err == 0);
  }

  // wait for all threads to be ready
  for (u32 i = 0; i < threadc; i++)
    assert(sema_wait(&sem_ready));
  //tlog("signalling sem_go");
  assert(sema_signal(&sem_go, threadc));

  // wait for all threads to exit
  for (u32 i = 0; i < threadc; i++) {
    test_thread_t* t = &threadv[i];
    int result = 123;
    int err = thrd_join(t->t, &result);
    assert(err == 0);
    assert(result == 0);
  }

  bump_allocator_t* a = BUMPALLOC_OF_MEMALLOC(ma);

  // check integrity by making sure that every thread's allocation is
  // accounted for in a slab of the allocator
  for (u32 i = 0; i < threadc; i++) {
    test_thread_t* t = &threadv[i];
    mem_t m1 = ATOMIC_LOAD(&t->m1);
    usize nslabs = 0;
    slab_t* slab = ATOMIC_LOAD(&a->tail);
    for (;;) {
      nslabs++;

      if (m1.p > (void*)slab && m1.p < (void*)slab + slab->size)
        break; // found

      if (slab == &a->head) {
        // reached the end; not found
        tlog("%p not found. The following %zu slabs exist in ma:", m1.p, nslabs);
        for (slab_t* slab = a->tail; ; slab = slab->prev) {
          tlog("  slab %3zu: %p .. %p", nslabs--, slab, (void*)slab + slab->size);
          if (slab == &a->head)
            break;
        }
        assertf(0, "%p not found", m1.p);
        break;
      }

      assertf(slab->prev != NULL, "slab(%p)->prev is NULL", slab);
      slab = slab->prev;
    }
  }

  // dispose
  sema_dispose(&sem_go);
  sema_dispose(&sem_ready);
  memalloc_bump2_dispose(ma);
  mem_freetv(memalloc_default(), threadv, threadc);

  log("%s: PASSED", __FUNCTION__);
}


#endif // CO_ENABLE_TESTS
