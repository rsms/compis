// AST encoder and decoder
// SPDX-License-Identifier: Apache-2.0
/*

An encoder can be used multiple times, e.g.

  astenc = astencoder_create(ma);
  for ( ... ) {
    astencoder_begin(astenc, compiler);
    astencoder_add_ast(astenc, node, flags);
    astencoder_encode(astenc, outbuf);
  }
  astencoder_free(astenc);

A decoder has a different API since it is usually paused and resumed
between learning what packages are imported, loading those packages
and decoding the AST (which refers to the imported packages.)

  astdec = astdecoder_open(ma, ast_ma, locmap, srcname, src, srclen);

  pkg_t pkg = {0};
  astdecoder_decode_header(astdec, &pkg);
  // load imports here

  node_t** pkgdeclv;
  u32 pkgdeclc;
  astdecoder_decode_ast(astdec, &pkgdeclv, &pkgdeclc);

  astdecoder_close(astdec);

*/
#pragma once
#include "ast.h"
ASSUME_NONNULL_BEGIN

typedef struct astencoder_ astencoder_t;
typedef struct astdecoder_ astdecoder_t;


// flags for astencoder_add_ast
#define ASTENCODER_PUB_API (1u << 0) // encode a public API


astencoder_t* nullable astencoder_create(compiler_t* c);
void astencoder_free(astencoder_t* a);

void astencoder_begin(astencoder_t* a, const pkg_t* pkg);
err_t astencoder_add_ast(astencoder_t* a, const node_t* n, u32 flags);
err_t astencoder_add_srcfileid(astencoder_t* a, u32 srcfileid);
err_t astencoder_add_srcfile(astencoder_t* a, const srcfile_t* srcfile);
err_t astencoder_encode(astencoder_t* a, buf_t* outbuf);


astdecoder_t* nullable astdecoder_open(
  compiler_t* c,
  memalloc_t  ast_ma,
  const char* srcname,
  const u8*   src,
  usize       srclen);

void astdecoder_close(astdecoder_t*);
const char* astdecoder_srcname(const astdecoder_t*);
memalloc_t astdecoder_ast_ma(const astdecoder_t*);

err_t astdecoder_decode_header(astdecoder_t* d, pkg_t* pkg, u32* importcount);
err_t astdecoder_decode_imports(
  astdecoder_t* d, pkg_t* pkg, sha256_t* nullable api_sha256v);
err_t astdecoder_decode_ast(astdecoder_t* d, node_t** resultv[], u32* resultc);


ASSUME_NONNULL_END
