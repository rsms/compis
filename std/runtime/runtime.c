#include <coprelude.h>
#include <stdio.h>

typedef struct {u64 len; const u8* ptr;} u8_slice_t;
typedef u8_slice_t str_t; // co "str" is an alias of "&[u8]"

void __co_panic(const char* message) {
  printf("panic: %s\n", message);
  __builtin_abort();
}

void panic(str_t msg) {
  fwrite("panic: ", __builtin_strlen("panic: "), 1, stderr);
  fwrite(msg.ptr, msg.len, 1, stderr);
  putc('\n', stderr);
  fflush(stderr);
  __builtin_abort();
}

void say_hello() {
  puts("hello");
}

/*#ifdef __APPLE__
  #define strong_alias(name, aliasname) \
    __asm__(".globl _" #aliasname); \
    __asm__(".set _" #aliasname ", _" #name); \
    extern __typeof(name) aliasname
#else
  #define strong_alias(name, aliasname) \
    extern __typeof__(name) aliasname __attribute__((__alias__(#name)))
#endif

#define weak_alias(name, aliasname) \
  extern __typeof(name) aliasname __attribute__((__weak__, __alias__(#name)))

strong_alias(__co_panic, panic);*/
