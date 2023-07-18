// SPDX-License-Identifier: Apache-2.0
#pragma once
ASSUME_NONNULL_BEGIN

//———————————————————————————————————————————————————————————————————————————————————————
// encoder

typedef struct {
  memalloc_t        ma;
  array_type(mem_t) tmpallocs;  // temporary allocations
  nodearray_t       nodelist;   // all nodes
  u32array_t        rootlist;   // indices in nodelist of root nodes
  u32array_t        srcfileids; // unique loc_t srcfile IDs
  map_t             nodemap;    // maps {node_t* => uintptr nodelist index}
  ptrarray_t        symmap;     // maps {sym_t => u32 index} (sorted set)
  usize             symsize;    // total length of all symbol characters
  locmap_t*         locmap;     // locmap in which all IDs in srcfileids are registered
  bool              oom;        // true if memory allocation failed (internal state)
} astencode_t;

// flags
#define AENC_PUB_API (1u << 0) // encode a public API

err_t astencode_init(astencode_t* a, memalloc_t ma, locmap_t* locmap);
void astencode_reset(astencode_t* a, locmap_t* locmap);
void astencode_dispose(astencode_t* a);

err_t astencode_add_ast(astencode_t* a, const node_t* n, u32 flags);
err_t astencode_encode(astencode_t* a, buf_t* outbuf);

//———————————————————————————————————————————————————————————————————————————————————————
// decoder

err_t astdecode(
  memalloc_t ma,     // generic allocator (must match that used for locmap)
  memalloc_t ast_ma, // allocator for resulting AST
  pkg_t* pkg,
  locmap_t* locmap,
  const char* srcname, const u8* src, usize srclen,
  node_t*** resultv, u32* resultc  // output result
);

ASSUME_NONNULL_END
