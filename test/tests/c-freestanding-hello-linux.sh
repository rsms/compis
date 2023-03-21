# Hello world for linux x86_64 without any libraries.

cat << END > hello.c
#include <stddef.h> // part of the compiler resource headers, not libc

// syscall numbers for linux x86_64
#define SC_WRITE 1
#define SC_EXIT  60

static int write(int fd, const void *buf, size_t size) {
  long result;
  __asm__ __volatile__(
    "syscall"
    : "=a"(result)
    : "0"(SC_WRITE), "D"(fd), "S"(buf), "d"(size)
    : "cc", "rcx", "r11", "memory");
  return result;
}

static void exit(int code) {
  __asm__ __volatile__(
    "syscall"
    :
    : "a"(SC_EXIT)
    : "cc", "rcx", "r11", "memory");
  __builtin_unreachable(); // syscall above never returns
}

void _start() {
  char text[] = "Hello, world!\n";
  long n = write(1, text, sizeof(text) - 1);
  exit(n == sizeof(text) - 1 ? 0 : 1);
}
END

# note: we can't link "in one go" since we don't target anything
cc --target=x86_64-none -c hello.c -o hello.o
ld.lld hello.o -o hello

# maybe run it
if [ "$(uname -sm)" = "Linux x86_64" ]; then
  ./hello
elif command -v blink >/dev/null &&
     blink -h | grep -q 'enable system call logging'
then
  blink hello
fi
