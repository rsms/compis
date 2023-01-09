#include <stdio.h>
#include <stdlib.h> // atol

// "import" function defined in foo.co
long foo(long x, long y);

// "export" function for use in foo.co
void printint(long v) {
  printf("%ld\n", v);
}

int main(int argc, const char** argv) {
  printf("Hello world! foo(2,3) => %ld\n", foo(2, 3));
  return 0;
}
