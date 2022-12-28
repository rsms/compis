#include <stdio.h>
#include <c0prelude.h>

// defined in foo.co
int main·foo(int x,int y);

void main·printu32(u32 v) {
  printf("%u\n", v);
}

int main(int argc, const char* argv[argc+1]) {
  printf("Hello world! main·foo(2,3) => %d\n", main·foo(2, 3));
  return 0;
}
