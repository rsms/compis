#include <stdio.h>
#include <stdlib.h> // atoi

// foo defined in foo.co
long foo(long x, long y);

void printu64(unsigned long long v) {
  printf("%llu\n", v);
}

int main(int argc, const char* argv[argc+1]) {
  long x = 2;
  if (argc > 1)
    x = atol(argv[1]);
  printf("Hello world! foo(%ld,3) => %ld\n", x, foo(x, 3));
  return 0;
}
