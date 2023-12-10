// SPDX-License-Identifier: Apache-2.0
#include "colib.h"
#include "tmpbuf.h"

static _Thread_local buf_t _tmpbuf[3];

buf_t* tmpbuf_get(u32 bufindex) {
  assert(bufindex < countof(_tmpbuf));
  buf_t* buf = &_tmpbuf[bufindex];
  buf->ma = memalloc_ctx();
  buf->oom = false;
  buf_clear(buf);
  return buf;
}

void tmpbuf_init(memalloc_t ma) {
  for (usize i = 0; i < countof(_tmpbuf); i++)
    buf_init(&_tmpbuf[i], ma);
}
