#include <stdio.h>

// defined in foo.co
int foo();

int main(int argc, const char* argv[argc+1]) {
  printf("Hello world! foo() => %d\n", foo());
  return 0;
}
