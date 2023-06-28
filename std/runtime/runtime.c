#include "runtime.h"
#include <stdio.h>

_Noreturn void __co_panic(__co_str_t msg) {
  fwrite("panic: ", __builtin_strlen("panic: "), 1, stderr);
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


void say_hello() {
  puts("hello");
}
