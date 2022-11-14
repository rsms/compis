#include <stdio.h>

// defined in foo.co
int foo(int x,int y);

int main(int argc, const char* argv[argc+1]) {
  printf("Hello world! foo() => %d\n", foo(1, 3));
  return 0;
}
