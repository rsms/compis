#include <stdio.h>

// defined in foo.co
int main·foo(int x,int y);

int main(int argc, const char* argv[argc+1]) {
  printf("Hello world! main·foo(2,3) => %d\n", main·foo(2, 3));
  return 0;
}
