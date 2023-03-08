#define import extern
#define export __attribute__((visibility("default")))

import void print(const char* cstr);

__attribute__((constructor))
static void myctor() {
  print("constructor called");
}

export int main(int argc, char* argv[]) {
  print("Hello world from main()");
  return 0;
}
