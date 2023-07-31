// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "buf.h"
ASSUME_NONNULL_BEGIN

// temporary buffers
// get_tmpbuf returns a thread-local general-purpose temporary buffer
void tmpbuf_init(memalloc_t);
buf_t* tmpbuf_get(u32 bufindex /* [0-2) */);

ASSUME_NONNULL_END
