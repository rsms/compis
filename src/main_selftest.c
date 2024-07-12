#include "colib.h"


u32 unittest_runall(); // unittest.c


int main_selftest(int argc, char* argv[]) {

  // run all integrated unit tests (defined with UNITTEST_DEF)
  if (unittest_runall())
    return 1;

  return 0;
}
