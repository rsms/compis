// system virtual memory
// SPDX-License-Identifier: Apache-2.0
#include "colib.h"

#include <fcntl.h>
#include <errno.h>
#include <sys/mman.h> // mmap

#ifdef DEBUG
  #include <string.h> // strerror
#endif


#ifdef _WIN32
  #define WIN32_LEAN_AND_MEAN
  #include <windows.h>
  #define MEM_PAGESIZE malloc_getpagesize
#elif defined(malloc_getpagesize)
  #define MEM_PAGESIZE malloc_getpagesize
#else
  #include <unistd.h>
  #ifdef _SC_PAGESIZE  /* some SVR4 systems omit an underscore */
    #ifndef _SC_PAGE_SIZE
      #define _SC_PAGE_SIZE _SC_PAGESIZE
    #endif
  #endif
  #ifdef _SC_PAGE_SIZE
    #define MEM_PAGESIZE sysconf(_SC_PAGE_SIZE)
  #elif defined(BSD) || defined(DGUX) || defined(R_HAVE_GETPAGESIZE)
    extern size_t getpagesize();
    #define MEM_PAGESIZE getpagesize()
  #else
    #include <sys/param.h>
    #ifdef EXEC_PAGESIZE
      #define MEM_PAGESIZE EXEC_PAGESIZE
    #elif defined(NBPG)
      #ifndef CLSIZE
        #define MEM_PAGESIZE NBPG
      #else
        #define MEM_PAGESIZE (NBPG * CLSIZE)
      #endif
    #elif defined(NBPC)
        #define MEM_PAGESIZE NBPC
    #elif defined(PAGESIZE)
      #define MEM_PAGESIZE PAGESIZE
    #endif
  #endif
  #include <sys/types.h>
  #include <sys/mman.h>
  #include <sys/resource.h>
  #if defined(__MACH__) && defined(__APPLE__)
    #include <mach/vm_statistics.h>
    #include <mach/vm_prot.h>
  #endif
  #ifndef MAP_ANONYMOUS
    #ifdef MAP_ANON
      #define MAP_ANONYMOUS MAP_ANON
    #else
      #define MAP_ANONYMOUS 0
    #endif
  #endif
  // MAP_NORESERVE flag says "don't reserve needed swap area"
  #ifndef MAP_NORESERVE
    #define MAP_NORESERVE 0
  #endif
  #define HAS_MMAP
#endif // _WIN32

#ifndef MEM_PAGESIZE
  // fallback value
  #define MEM_PAGESIZE ((usize)4096U)
#endif


usize sys_pagesize() {
  return (usize)MEM_PAGESIZE;
}


static mem_t sys_vm_alloc(void* nullable at_addr, usize nbytes) {
  if (nbytes == 0) {
    dlog("mmap failed: zero size requested");
    return (mem_t){0};
  }

  int protection = PROT_READ | PROT_WRITE;
  int flags = MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE;

  nbytes = ALIGN2(nbytes, sys_pagesize());
  void* p = mmap(at_addr, nbytes, protection, flags, -1, 0);

  if UNLIKELY(p == MAP_FAILED || p == NULL) {
    dlog("mmap failed (errno %d %s)", errno, strerror(errno));
    return (mem_t){0};
  }

  return (mem_t){ .p = p, .size = nbytes };
}


err_t sys_vm_free(mem_t m) {
  if (munmap(m.p, m.size) == 0)
    return 0;
  return err_errno();
}
