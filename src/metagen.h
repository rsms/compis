// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "compiler.h"
ASSUME_NONNULL_BEGIN

err_t metagen(
  buf_t* outbuf,
  compiler_t* c,
  const pkg_t* pkg,
  const unit_t*const* unitv, u32 unitc);

ASSUME_NONNULL_END
