// future_t supports a single producer multiple consumer scenario
// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "thread.h"
ASSUME_NONNULL_BEGIN

typedef struct {
  sema_t         sem;
  _Atomic(err_t) status; // 0: not started, 1: processing, 2: done(ok), <0: done(error)
} future_t;

err_t future_init(future_t* p);
void future_dispose(future_t* p);

// future_trywait returns true immediately if future finished,
// in which case *result_errp is set to the value of result_err passed
// to future_finalize.
// Returns false if future_finalize has not been called.
bool future_trywait(future_t* p, err_t* result_errp);

// future_wait waits for p to finish production.
// DEADLOCKS if future_finalize is never called.
// Returns the value of result_err passed to future_finalize.
err_t future_wait(future_t* p);

// future_acquire returns true exactly once, for one thread.
// If this function returns true, the caller must (eventually) call
// future_finalize to avoid deadlock from future_wait.
// If this function returns false, the caller may call future_wait to
// wait for the result.
bool future_acquire(future_t* p);

// future_finalize must only be called after a successful
// call to future_acquire and only called once.
void future_finalize(future_t* p, err_t result_err);

ASSUME_NONNULL_END
