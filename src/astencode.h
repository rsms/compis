// SPDX-License-Identifier: Apache-2.0
#pragma once
ASSUME_NONNULL_BEGIN

// typedef struct { usize start, end; } sizerange_t;

typedef struct {
  memalloc_t ma;
  ptrarray_t nodelist;
  u32array_t rootlist; // indices in nodelist of root nodes
  map_t      nodemap;  // maps {node_t* => uintptr nodelist index}
  ptrarray_t children; // work set
} astencode_t;

err_t astencode_init(astencode_t* ae, memalloc_t ma);
void astencode_dispose(astencode_t* ae);
err_t astencode_add_ast(astencode_t* ae, const node_t* n);
err_t astencode_encode(astencode_t* ae, buf_t* outbuf);

// if astencode_begin fails, the buf_t is not consumed and the caller
// regains ownership.

// if you do not call astencode_end after calling astencode_begin,
// the outbuf_consume buffer will be disposed of by the next call to
// astencode_begin or astencode_dispose.

ASSUME_NONNULL_END
