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
  #undef _
  NK_UNKNOWN, NK__UNKNOWN = NK_UNKNOWN + strlen(NK_UNKNOWN_STR),
};
static const struct {
  u8   offs[NODEKIND_COUNT + 1]; // index into strs
  char strs[];
} nodekind_strtab = {
  { // get offset from enum
    #define _(NAME) NK_##NAME,
    FOREACH_NODEKIND(_)
    #undef _
    NK_UNKNOWN,
  }, {
    #define _(NAME) #NAME "\0"
    FOREACH_NODEKIND(_)
    #undef _
    NK_UNKNOWN_STR
  }
};

#define TK_UNKNOWN_STR "TYPE?"
enum {
  #define _(NAME) TK_##NAME, TK__##NAME = TK_##NAME + strlen(#NAME),
  FOREACH_TYPEKIND(_)
  #undef _
  TK_UNKNOWN, TK__UNKNOWN = TK_UNKNOWN + strlen(TK_UNKNOWN_STR),
};
static const struct {
  u8   offs[TYPEKIND_COUNT + 1];
  char strs[];
} typekind_strtab = {
  {
    #define _(NAME) TK_##NAME,
    FOREACH_TYPEKIND(_)
    #undef _
    TK_UNKNOWN,
  }, {
    #define _(NAME) #NAME "\0"
    FOREACH_TYPEKIND(_)
    #undef _
    TK_UNKNOWN_STR
  }
};


#define STRTAB_GET(strtab, kind) \
  &(strtab).strs[ (strtab).offs[ MIN((kind), countof(strtab.offs)-1) ] ]


const char* node_name(const node_t* n) {
  return STRTAB_GET(nodekind_strtab, n->kind);
}

const char* type_name(const type_t* t) {
  return STRTAB_GET(typekind_strtab, t->kind);
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
  abuf_c(s, ')')


static void repr_type(abuf_t* s, const type_t* t, usize indent, reprflag_t fl) {
  const char* kindname = STRTAB_GET(typekind_strtab, t->kind);
  REPR_BEGIN(kindname);
  switch ((enum typekind)t->kind) {
    case TYPEKIND_COUNT:
    case TYPE_VOID:
    case TYPE_BOOL:
    case TYPE_INT:
    case TYPE_I8:
    case TYPE_I16:
    case TYPE_I32:
    case TYPE_I64:
    case TYPE_F32:
    case TYPE_F64:
      break;
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


static void repr(abuf_t* s, const node_t* n, usize indent, reprflag_t fl) {
  const char* kindname = STRTAB_GET(nodekind_strtab, n->kind);
  REPR_BEGIN(kindname);

  switch (n->kind) {

  case EXPR_BLOCK:
  case NODE_UNIT:
    for (usize i = 0; i < n->children.len; i++) {
      abuf_c(s, ' ');
      repr(s, n->children.v[i], indent, fl);
    }
    break;

  case EXPR_FUN:
    if (n->fun.name)
      abuf_c(s, ' '), abuf_str(s, n->fun.name->strval);
    abuf_c(s, ' '), repr_type(s, n->fun.result_type, indent, fl);
    if (n->fun.body)
      abuf_c(s, ' '), repr(s, n->fun.body, indent, fl);
    break;

  case EXPR_PREFIXOP:
  case EXPR_POSTFIXOP:
    abuf_c(s, ' '), abuf_str(s, tok_repr(n->op1.op));
    abuf_c(s, ' '), repr(s, n->op1.expr, indent, fl);
    break;

  case EXPR_INFIXOP:
    abuf_c(s, ' '), abuf_str(s, tok_repr(n->op2.op));
    abuf_c(s, ' '), repr(s, n->op2.left, indent, fl);
    abuf_c(s, ' '), repr(s, n->op2.right, indent, fl);
    break;

  case EXPR_INTLIT:
    abuf_c(s, ' '), abuf_u64(s, n->intval, 10); break;

  case EXPR_ID:
    abuf_c(s, ' '), abuf_str(s, n->strval); break;

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


void type_free(memalloc_t ast_ma, type_t* t) {
}


void node_free(memalloc_t ast_ma, node_t* n) {
/*  if (n == last_resort_node)
    return;

  switch (n->kind) {
  case NID:
    mem_freex(ast_ma, MEM(n->strval, strlen(n->strval))); break;
  case NFUN:
    if (n->name) node_free(ast_ma, n->name);
    if (n->body) node_free(ast_ma, n->body);
    break;
  case NBLOCK:
  case NUNIT:
    for (node_t* cn = n->children.head; cn; cn = cn->next) {
      abuf_c(s, ' ');
      repr(s, cn, indent, fl);
    }
    break;
  default:
    break;
  }

  for (node_t* cn = n->children.head; cn;) {
    node_t* cn2 = cn->next;
    node_free(ast_ma, cn);
    cn = cn2;
  }
  if (node_has_strval(n)) {
    mem_t m = MEM(n->strval, strlen(n->strval));
    mem_free(ast_ma, &m);
  }
  mem_freet(ast_ma, n);*/
}
