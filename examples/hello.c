#include <stdio.h>
#include <stdlib.h> // atoi

// main.foo defined in foo.co
int NfM4main3foo(int x,int y);

// main.printu32
void NfM4main8printu32(unsigned int v) {
  printf("%u\n", v);
}

int main(int argc, const char* argv[argc+1]) {
  int x = 2;
  if (argc > 1)
    x = atoi(argv[1]);
  printf("Hello world! main.foo(%d,3) => %d\n", x, NfM4main3foo(x, 3));
  return 0;
}
