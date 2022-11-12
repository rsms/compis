// diagnostics reporting
#include "c0lib.h"
#include "compiler.h"
#include "abuf.h"


typedef enum reprflag {
  REPRFLAG_HEAD = 1 << 0, // is list head
} reprflag_t;


// node kind string table with compressed indices (compared to table of pointers.)
// We end up with something like this; one string with indices:
//   enum {
//     NK_NBAD, NK__NBAD = NK_NBAD + strlen("NBAD"),
//     NK_NCOMMENT, NK__NCOMMENT = NK_NCOMMENT + strlen("NCOMMENT"),
//     NK_NUNIT, NK__NUNIT = NK_NUNIT + strlen("NUNIT"),
//     NK_NFUN, NK__NFUN = NK_NFUN + strlen("NFUN"),
//     NK_NBLOCK, NK__NBLOCK = NK_NBLOCK + strlen("NBLOCK"),
//     NK_UNKNOWN, NK__UNKNOWN = NK_UNKNOWN + strlen("N?"),
//   };
//   static const struct { u8 offs[NODEKIND_COUNT]; char strs[]; } nodekind_strtab = {
//     { NK_NBAD, NK_NCOMMENT, NK_NUNIT, NK_NFUN, NK_NBLOCK },
//     { "NBAD\0NCOMMENT\0NUNIT\0NFUN\0NBLOCK\0N?" }
//   };
#define NK_UNKNOWN_STR "NODE?"
enum {
  #define _(NAME) NK_##NAME, NK__##NAME = NK_##NAME + strlen(#NAME),
  FOREACH_NODEKIND(_)
  FOREACH_NODEKIND_TYPE(_)
  #undef _
  NK_UNKNOWN, NK__UNKNOWN = NK_UNKNOWN + strlen(NK_UNKNOWN_STR),
};
static const struct {
  int  offs[NODEKIND_COUNT + 1]; // index into strs
  char strs[];
} nodekind_strtab = {
  { // get offset from enum
    #define _(NAME) NK_##NAME,
    FOREACH_NODEKIND(_)
    FOREACH_NODEKIND_TYPE(_)
    #undef _
    NK_UNKNOWN,
  }, {
    #define _(NAME) #NAME "\0"
    FOREACH_NODEKIND(_)
    FOREACH_NODEKIND_TYPE(_)
    #undef _
    NK_UNKNOWN_STR
  }
};


#define STRTAB_GET(strtab, kind) \
  &(strtab).strs[ (strtab).offs[ MIN((kind), countof(strtab.offs)-1) ] ]


const char* nodekind_name(nodekind_t kind) {
  return STRTAB_GET(nodekind_strtab, kind);
}


#define REPR_BEGIN(kindname) ({ \
  if ((fl & REPRFLAG_HEAD) == 0) \
    abuf_c(s, '\n'), abuf_fill(s, ' ', indent); \
  fl &= ~REPRFLAG_HEAD; \
  abuf_c(s, '('); \
  indent += 2; \
  abuf_str(s, (kindname)); \
})


#define REPR_END() \
  ( abuf_c(s, ')'), indent -= 2 )


static void repr(abuf_t* s, const node_t* n, usize indent, reprflag_t fl);


static void repr_type(abuf_t* s, const type_t* t, usize indent, reprflag_t fl) {
  REPR_BEGIN(nodekind_name(t->kind));
  switch (t->kind) {
    case TYPE_ARRAY:
    case TYPE_ENUM:
    case TYPE_FUNC:
    case TYPE_PTR:
    case TYPE_STRUCT:
      dlog("TODO subtype(s)");
      break;
  }
  REPR_END();
}


static void repr_local(abuf_t* s, const local_t* n, usize indent, reprflag_t fl) {
  assert(n->kind == NODE_LOCAL);
  REPR_BEGIN(n->name);
  abuf_c(s, ' ');
  repr_type(s, n->type, indent, fl | REPRFLAG_HEAD);
  REPR_END();
}


static void repr_fun(abuf_t* s, const fun_t* n, usize indent, reprflag_t fl) {
  if (n->name) {
    abuf_c(s, ' '), abuf_str(s, n->name->sym);
  }
  {
    REPR_BEGIN("params");
    for (u32 i = 0; i < n->params.len; i++) {
      abuf_c(s, ' ');
      repr_local(s, (local_t*)n->params.v[i], indent, fl);
    }
    REPR_END();
  }
  abuf_c(s, ' '), repr_type(s, n->result_type, indent, fl);
  if (n->body)
    abuf_c(s, ' '), repr(s, (node_t*)n->body, indent, fl);
}


static void repr_nodearray(
  abuf_t* s, const ptrarray_t* nodes, usize indent, reprflag_t fl)
{
  for (usize i = 0; i < nodes->len; i++) {
    abuf_c(s, ' ');
    repr(s, nodes->v[i], indent, fl);
  }
}


static void repr(abuf_t* s, const node_t* n, usize indent, reprflag_t fl) {
  const char* kindname = STRTAB_GET(nodekind_strtab, n->kind);
  REPR_BEGIN(kindname);

  switch (n->kind) {

  case NODE_UNIT:
    repr_nodearray(s, &((unit_t*)n)->children, indent, fl); break;

  case EXPR_BLOCK:
    repr_nodearray(s, &((block_t*)n)->children, indent, fl); break;

  case EXPR_FUN:
    return repr_fun(s, (fun_t*)n, indent, fl);

  case EXPR_PREFIXOP:
  case EXPR_POSTFIXOP: {
    op1expr_t* op = (op1expr_t*)n;
    abuf_c(s, ' '), abuf_str(s, tok_repr(op->op));
    abuf_c(s, ' '), repr(s, (node_t*)op->expr, indent, fl);
    break;
  }

  case EXPR_INFIXOP: {
    op2expr_t* op = (op2expr_t*)n;
    abuf_c(s, ' '), abuf_str(s, tok_repr(op->op));
    abuf_c(s, ' '), repr(s, (node_t*)op->left, indent, fl);
    abuf_c(s, ' '), repr(s, (node_t*)op->right, indent, fl);
    break;
  }

  case EXPR_INTLIT:
    abuf_c(s, ' '), abuf_u64(s, ((intlitexpr_t*)n)->intval, 10); break;

  case EXPR_ID:
    abuf_c(s, ' '), abuf_str(s, ((idexpr_t*)n)->sym); break;

  }
  REPR_END();
}


err_t node_repr(buf_t* buf, const node_t* n) {
  usize needavail = 4096;
  for (;;) {
    buf_reserve(buf, needavail);
    abuf_t s = abuf_make(buf->p, buf->cap);
    repr(&s, n, 0, REPRFLAG_HEAD);
    usize len = abuf_terminate(&s);
    if (len < needavail) {
      buf->len += len;
      break;
    }
    needavail = len + 1;
  }
  return 0;
}
