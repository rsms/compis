// package metadata
// SPDX-License-Identifier: Apache-2.0
#include "colib.h"
#include "compiler.h"
#include "metagen.h"
#include "astencode.h"

// format: (key value ...)


typedef struct {
  compiler_t*  c;
  const pkg_t* pkg;
  usize        outbuf_startlen;
  buf_t        outbuf;
  err_t        err;
} mg_t;

typedef enum {
  MGFLAG_HEAD = 1 << 0, // is list head
} mgflag_t;

#define INDENT 2

#define GPARAMS        mg_t* g, usize indent, mgflag_t flags
#define GARGS          g, indent, flags
#define GARGSFL(flags) g, indent, flags

#define REGOOM(boolexpr) ( \
  g->err = (((err_t)!(boolexpr)) * ErrNoMem * (err_t)(!g->err)) + g->err \
)

#define CHAR(ch)             REGOOM( buf_push(&g->outbuf, (ch)) )
#define PRINT(cstr)          REGOOM( buf_print(&g->outbuf, (cstr)) )
#define PRINTN(cstr, len)    REGOOM( buf_append(&g->outbuf, (cstr), (len)) )
#define PRINTF(fmt, args...) REGOOM( buf_printf(&g->outbuf, (fmt), ##args) )
#define PRINTSTR(str)        PRINTN((str).p, (str).len)
#define FILL(byte, len)      REGOOM( buf_fill(&g->outbuf, (byte), (len)) )

#define NEWLINE() ( CHAR('\n'), FILL(' ', indent) )

#define OPEN(opench, name) ({ \
  if ((flags & MGFLAG_HEAD) == 0) { \
    if (g->outbuf.len > g->outbuf_startlen) \
      NEWLINE(); \
    indent += INDENT; \
  } \
  flags &= ~MGFLAG_HEAD; \
  CHAR(opench); \
  PRINT(name); \
})

#define CLOSE(closech) ( CHAR((closech)), indent -= INDENT )


static void quotebytes(mg_t* g, const void* nullable p, usize len) {
  CHAR('"');
  if (len > 0)
    REGOOM( buf_appendrepr(&g->outbuf, p, len) );
  CHAR('"');
}

static void quotestr(mg_t* g, str_t s) { quotebytes(g, s.p, s.len); }
//static void quoteslice(mg_t* g, slice_t s) { quotebytes(g, s.p, s.len); }
static void quotecstr(mg_t* g, const char* s) { quotebytes(g, s, strlen(s)); }


static void metagen_pkginfo(GPARAMS) {
  OPEN('(', "path "), quotestr(g, g->pkg->path), CLOSE(')');
  OPEN('(', "dir "), quotestr(g, g->pkg->dir), CLOSE(')');
  //OPEN('(', "copath "), quoteslice(g, g->pkg->rootdir), CLOSE(')');

  OPEN('(', "srcfiles");
  for (u32 i = 0; i < g->pkg->files.len; i++) {
    const srcfile_t* f = &g->pkg->files.v[i];
    OPEN('(', ""), quotestr(g, f->name);
    PRINTF(" %llu", f->mtime);
    CLOSE(')');
  }
  CLOSE(')');

  OPEN('(', "imports");
  for (u32 i = 0; i < g->pkg->imports.len; i++) {
    const pkg_t* dep_pkg = g->pkg->imports.v[i];
    NEWLINE(), quotestr(g, dep_pkg->path);
  }
  CLOSE(')');
}


static void metagen_pkginfo_2(GPARAMS) {
  PRINT("path    = "), quotestr(g, g->pkg->path), CHAR('\n');
  PRINT("dir     = "), quotestr(g, g->pkg->dir), CHAR('\n');

  PRINT("imports = [");
  for (u32 i = 0; i < g->pkg->imports.len; i++) {
    const pkg_t* dep_pkg = g->pkg->imports.v[i];
    PRINT("\n  "), quotestr(g, dep_pkg->path);
    if (i+1 < g->pkg->imports.len)
      PRINT(",");
  }
  PRINT("]\n");

  PRINT("\n[srcfiles]\n");
  for (u32 i = 0; i < g->pkg->files.len; i++) {
    const srcfile_t* f = &g->pkg->files.v[i];
    quotestr(g, f->name), PRINTF(".mtime = %llu\n", f->mtime);
  }
}


static void fun_proto(GPARAMS, const fun_t* fun) {
  funtype_t* ft = (funtype_t*)fun->type;

  assertnotnull(fun->name);

  OPEN('(', "fun\n");

  err_t err = node_fmt(&g->outbuf, (const node_t*)fun, U32_MAX);


  // PRINT(" (");
  // for (u32 i = 0; i < n->params.len; i++) {
  //   if (i) CHAR(' ');
  //   repr(RARGS, n->params.v[i]);
  // }
  // CHAR(')');
  // repr_type(RARGS, n->result);

  // type(g, ft->result);

  CHAR('\n');
  CLOSE(')');

  // if (ft->params.len > 0) {
  //   g->scopenest++;
  //   for (u32 i = 0; i < ft->params.len; i++) {
  //     local_t* param = ft->params.v[i];
  //     if (i) PRINT(", ");
  //     // if (!type_isprim(param->type) && !param->ismut)
  //     //   PRINT("const ");
  //     type(g, param->type);
  //     if (noalias(param->type))
  //       PRINT(CO_INTERNAL_PREFIX "noalias");
  //     if (param->name && param->name != sym__) {
  //       CHAR(' ');
  //       PRINT(param->name);
  //     }
  //   }
  //   g->scopenest--;
  // } else {
  //   PRINT("void");
  // }
  // CHAR(')');
}


static void metagen_api_toplevel(GPARAMS, const node_t* n) {
  assertf(n->kind == STMT_TYPEDEF
       || n->kind == EXPR_FUN
    , "unexpected top-level node %s", nodekind_name(n->kind));

  g->err = node_fmt(&g->outbuf, n, U32_MAX);
  CHAR('\n');
}


static err_t add_decl(map_t* decls, memalloc_t ma, const node_t* n) {
  void** vp = map_assign_ptr(decls, ma, n);
  if (!vp)
    return ErrNoMem;
  if (!*vp) {
    *vp = (void*)n;
  }
  return 0;
}


static void metagen_api(GPARAMS, const unit_t*const* unitv, u32 unitc) {
  OPEN('(', "api\n\n");

  dlog("—————————————————— astencode ——————————————————");

  buf_t encoded_ast = buf_make(g->c->ma);

  // create AST encoder
  astencode_t astenc;
  if (( g->err = astencode_init(&astenc, g->c->ma) ))
    return; // avoid calling astvisit_dispose when astvisit_init failed

  // encode package
  #if 1
  for (u32 i = 0; i < unitc && g->err == 0; i++) {
    const unit_t* unit = unitv[i];
    for (u32 i = 0; i < unit->children.len && g->err == 0; i++) {
      const node_t* n = unit->children.v[i];
      if ((n->flags & NF_VIS_PUB) == 0)
        continue;
      g->err = astencode_add_ast(&astenc, n);
    }
  }
  if (g->err)
    panic("astencode_add_ast failed: %s", err_str(g->err));
  if (!g->err)
    g->err = astencode_encode(&astenc, &encoded_ast);
  #else
  if (( g->err = astencode_begin(&astenc, &encoded_ast) )) {
    buf_dispose(&encoded_ast);
  } else for (u32 i = 0; i < unitc && g->err == 0; i++) {
    const unit_t* unit = unitv[i];
    for (u32 i = 0; i < unit->children.len && g->err == 0; i++) {
      const node_t* n = unit->children.v[i];
      if ((n->flags & NF_VIS_PUB) == 0)
        continue;
      g->err = astencode(&astenc, n);
    }
    err_t err1 = astencode_end(&astenc, &encoded_ast);
    g->err = g->err ?: err1;
  }
  #endif

  // free AST encoder
  astencode_dispose(&astenc);

  // free buffer with encoded AST
  PRINTN(encoded_ast.p, encoded_ast.len); // XXX
  buf_dispose(&encoded_ast);

  #if 0

  map_t decls;
  if (!map_init(&decls, g->c->ma, 32)) {
    g->err = ErrNoMem;
    return;
  }

  for (u32 i = 0; i < unitc && g->err == 0; i++) {
    const unit_t* unit = unitv[i];
    for (u32 i = 0; i < unit->children.len && g->err == 0; i++) {
      const node_t* n = unit->children.v[i];
      if ((n->flags & NF_VIS_PUB) == 0)
        continue;
      // g->err = add_decl(&decls, g->c->ma, n);
      metagen_api_toplevel(GARGS, n);
    }
  }

  // TODO: topologically sort decls

  // for (const mapent_t* e = map_it(&decls); map_itnext(&decls, &e); )
  //   metagen_api_toplevel(GARGS, e->value);

  #endif

  CHAR('\n');
  CLOSE(')');
}


err_t metagen(
  buf_t* outbuf, compiler_t* c, const pkg_t* pkg, const unit_t*const* unitv, u32 unitc)
{
  mg_t g_ = {
    .c = c,
    .pkg = pkg,
    .outbuf = *outbuf,
    .outbuf_startlen = outbuf->len,
  };

  // GARGS
  mg_t* g = &g_;
  mgflag_t flags = 0;
  usize indent = 0;

  metagen_pkginfo(GARGS);
  metagen_api(GARGS, unitv, unitc);

  // add final linebreak, if needed
  if (!g->err && (g->outbuf.len == 0 || g->outbuf.chars[g->outbuf.len-1] != '\n'))
    CHAR('\n');

  dlog("——————————————————————\n%.*s\n——————————————————————",
    (int)g->outbuf.len, g->outbuf.chars);

  *outbuf = g->outbuf;
  return g->err;
}
