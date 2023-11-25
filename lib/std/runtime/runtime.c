#include "runtime.h"
#if !__STDC_HOSTED__
  #error Not yet implemented for freestanding
#else
  #include <stdio.h>
  #include <stdlib.h> // malloc et al
#endif

_Noreturn void __co_panic(__co_str msg) {
  fwrite("panic: ", strlen("panic: "), 1, stderr);
  fwrite(msg.ptr, msg.len, 1, stderr);
  putc('\n', stderr);
  fflush(stderr);
  __builtin_abort();
}

strong_alias(__co_panic, panic);

_Noreturn void __co_panic_out_of_bounds(void) {
  __co_panic(__CO_X_STR(u8"out of bounds access"));
}

_Noreturn void __co_panic_null(void) {
  __co_panic(__CO_X_STR(u8"null pointer"));
}


void* __co_mem_dup(const void* src, __co_uint size) {
  void* ptr = malloc(size);
  memcpy(ptr, src, size);
  // __builtin_printf("__co_mem_dup(%p, %lu) => %p\n", src, size, ptr);
  return ptr;
}

void __co_mem_free(void* ptr, __co_uint size) {
  // __builtin_printf("__co_mem_free(%p, %lu)\n", ptr, size);
  free(ptr);
}


void _print(__co_str msg) {
  fwrite(msg.ptr, msg.len, 1, stdout);
  fputc('\n', stdout);
}