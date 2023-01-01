// SPDX-License-Identifier: Apache-2.0
#include "colib.h"

#include <sys/wait.h> // waitpid


void promise_open(promise_t* p, pid_t pid) {
  assert(p->pid == 0);
  memset(p, 0, sizeof(*p));
  p->pid = pid;
}


void promise_open_done(promise_t* p, err_t result_err) {
  assert(p->pid == 0);
  memset(p, 0, sizeof(*p));
  p->pid = -1;
  p->err = result_err;
}


void promise_close(promise_t* p) {
  assert(p->pid != 0);
  p->pid = 0;
}


err_t promise_await(promise_t* p) {
  if (p->pid == 0)
    return ErrCanceled;
  if (p->pid < 0) {
    promise_close(p);
    return p->err;
  }
  int wstatus;
  waitpid(p->pid, &wstatus, 0);
  promise_close(p);
  return wstatus == 0 ? 0 : ErrInvalid;
}
