#include <stdio.h>
#include <c0prelude.h>

// main.foo defined in foo.co
int NfM4main3foo(int x,int y);

// main.printu32
void NfM4main8printu32(u32 v) {
  printf("%u\n", v);
}

int main(int argc, const char* argv[argc+1]) {
  printf("Hello world! main.foo(2,3) => %d\n", NfM4main3foo(2, 3));
  return 0;
}
