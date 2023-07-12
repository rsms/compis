// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "buf.h"
ASSUME_NONNULL_BEGIN

bool debug_histogram_fmt(buf_t* buf, usize* labels, usize* counts, usize len);

ASSUME_NONNULL_END
