// SPDX-License-Identifier: Apache-2.0
#include "colib.h"

err_t promise_await(promise_t* p) {
  if (p->await) {
    p->result = p->await(p->impl);
    p->await = NULL;
  }
  return p->result;
}
