// SPDX-License-Identifier: Apache-2.0
#include "colib.h"
#include <alloca.h>

void co_swap(void* a, void* b, usize size) {
  void* tmp = alloca(size);
  memcpy(tmp, a, size);
  memcpy(a, b, size);
  memcpy(b, tmp, size);
}
