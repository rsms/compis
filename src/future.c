// SPDX-License-Identifier: Apache-2.0
#include "colib.h"
#include "future.h"


err_t future_init(future_t* p) {
  return sema_init(&p->sem, 0);
}


void future_dispose(future_t* p) {
  assertf(p->status == 0 || sema_trywait(&p->sem), "future never finished");
  sema_dispose(&p->sem);
}


bool future_trywait(future_t* p, err_t* result_errp) {
  err_t status = AtomicLoadAcq(&p->status);
  if (status == 1 || status == 0)
    return false;
  *result_errp = (status == 2) ? 0 : status;
  return true;
}


err_t future_wait(future_t* p) {
  err_t status = AtomicLoadAcq(&p->status);
  if (status == 2)
    return 0;
  if (status < 0)
    return status;

  // another thread is (status 1)--or will be (status 0)--loading the package
  safecheckx(sema_wait(&p->sem));
  sema_signal(&p->sem, 1); // restore signal that we "took"

  // reload status
  status = AtomicLoadAcq(&p->status);
  return (status == 2) ? 0 : status;
}


bool future_acquire(future_t* p) {
  err_t status = AtomicLoadAcq(&p->status);
  if (status != 0)
    return false;
  return AtomicCAS(
    &p->status, &status, (err_t)1, memory_order_acq_rel, memory_order_relaxed);
}


void future_finalize(future_t* p, err_t result_err) {
  err_t status = 1;
  assertf(AtomicLoadAcq(&p->status) == status, "unbalanced future_begin/finish calls");
  AtomicStoreRel(&p->status, result_err == 0 ? 2 : result_err);
  sema_signal(&p->sem, 2); // yes, 2 signals
}