#include <stdio.h>

__attribute__((constructor)) static void myctor() {
  puts("constructor called");
}

__attribute__((visibility("default"))) int add(int x, int y) {
  return x + y;
}

int main(int argc, char* argv[]) {
  puts("Hello world!");
  return 0;
}
