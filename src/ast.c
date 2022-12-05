// diagnostics reporting
// SPDX-License-Identifier: Apache-2.0
#include "c0lib.h"
#include "compiler.h"
#include "abuf.h"


typedef enum reprflag {
  REPRFLAG_HEAD = 1 << 0, // is list head
  REPRFLAG_SHORT = 1 << 1,
} reprflag_t;


typedef struct {
  buf_t outbuf;
  err_t err;
  map_t seen;
} repr_t;


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


#define CHAR(ch) ( \
  buf_push(&r->outbuf, (ch)) ?: seterr(r, ErrNoMem) )

#define PRINT(cstr) ( \
  buf_print(&r->outbuf, (cstr)) ?: seterr(r, ErrNoMem) )

#define PRINTF(fmt, args...) ( \
  buf_printf(&r->outbuf, (fmt), ##args) ?: seterr(r, ErrNoMem) )

#define FILL(byte, len) ( \
  buf_fill(&r->outbuf, (byte), (len)) ?: seterr(r, ErrNoMem) )

#define INDENT 2

#define NEWLINE() \
  ( CHAR('\n'), FILL(' ', indent) )

#define REPR_BEGIN(opench, kindname) ({ \
  if ((fl & REPRFLAG_HEAD) == 0) { \
    NEWLINE(); \
    indent += INDENT; \
  } \
  fl &= ~REPRFLAG_HEAD; \
  CHAR(opench); \
  PRINT(kindname); \
})

#define REPR_END(closech) \
  ( CHAR((closech)), indent -= 2 )


const char* nodekind_name(nodekind_t kind) {
  return STRTAB_GET(nodekind_strtab, kind);
}


static void seterr(repr_t* r, err_t err) {
  if (!r->err)
    r->err = err;
}


#define RPARAMS repr_t* r, usize indent, reprflag_t fl
#define RARGS       r, indent, fl
#define RARGSFL(fl) r, indent, fl

static void repr(RPARAMS, const node_t* nullable n);
static void repr_type(RPARAMS, const type_t* t);


static bool seen(repr_t* r, const void* n) {
  if (nodekind_isbasictype(((const node_t*)n)->kind))
    return false;
  const void** vp = (const void**)map_assign_ptr(&r->seen, r->outbuf.ma, n);
  if (vp && !*vp) {
    *vp = n;
    return false;
  }
  if (!vp)
    seterr(r, ErrNoMem);
  CHAR('\'');
  return true;
}


static void repr_nodearray(RPARAMS, const ptrarray_t* nodes) {
  for (usize i = 0; i < nodes->len; i++) {
    CHAR(' ');
    repr(RARGS, nodes->v[i]);
  }
}


static void repr_typedef(RPARAMS, const typedef_t* n) {
  CHAR(' ');
  PRINT(n->name);
  CHAR(' ');
  repr_type(RARGS, n->type);
}


static void repr_struct(RPARAMS, const structtype_t* n, bool isnew) {
  if (n->name)
    CHAR(' '), PRINT(n->name);
  if (!isnew)
    return;
  for (u32 i = 0; i < n->fields.len; i++) {
    CHAR(' ');
    repr(RARGS, n->fields.v[i]);
  }
}


static void repr_fun(RPARAMS, const fun_t* n) {
  if (n->body)
    CHAR(' '), repr(RARGS, (node_t*)n->body);
}


static void repr_funtype(RPARAMS, const funtype_t* n) {
  PRINT(" (");
  for (u32 i = 0; i < n->params.len; i++) {
    if (i) CHAR(' ');
    repr(RARGS, n->params.v[i]);
  }
  CHAR(')');
  repr_type(RARGS, n->result);
}


static void repr_call(RPARAMS, const call_t* n) {
  CHAR(' ');
  repr(RARGSFL(fl | REPRFLAG_SHORT), (const node_t*)n->recv);
  if (n->args.len == 0)
    return;
  CHAR(' ');
  for (usize i = 0; i < n->args.len; i++) {
    if (i) CHAR(' ');
    repr(RARGS, (const node_t*)n->args.v[i]);
  }
}


static void repr_type(RPARAMS, const type_t* t) {
  REPR_BEGIN('<', nodekind_name(t->kind));
  bool isnew = !seen(r, t);
  switch (t->kind) {
  case TYPE_INT:
  case TYPE_I8:
  case TYPE_I16:
  case TYPE_I32:
  case TYPE_I64:
    PRINT(t->isunsigned ? " u" : " s");
    break;
  case TYPE_STRUCT:
    repr_struct(RARGS, (const structtype_t*)t, isnew);
    break;
  case TYPE_FUN:
    if (isnew) {
      repr_funtype(RARGS, (const funtype_t*)t);
    } else {
      CHAR('\'');
    }
    break;
  case TYPE_PTR:
    CHAR(' ');
    repr_type(RARGSFL(fl | REPRFLAG_HEAD), ((const ptrtype_t*)t)->elem);
    break;
  case TYPE_REF:
    CHAR(' ');
    if (((const reftype_t*)t)->ismut)
      PRINT("mut ");
    repr_type(RARGSFL(fl | REPRFLAG_HEAD), ((const reftype_t*)t)->elem);
    break;
  case TYPE_OPTIONAL:
    CHAR(' ');
    repr_type(RARGSFL(fl | REPRFLAG_HEAD), ((const opttype_t*)t)->elem);
    break;
  case TYPE_ARRAY:
  case TYPE_ENUM:
    dlog("TODO subtype %s", nodekind_name(t->kind));
    break;
  }
  REPR_END('>');
}


static void cleanup(RPARAMS, const ptrarray_t* cleanup) {
  if (cleanup->len == 0)
    return;
  REPR_BEGIN('(', "cleanup");
  for (u32 i = 0; i < cleanup->len; i++) {
    const local_t* owner = cleanup->v[i];
    CHAR(' ');
    PRINT(owner->name);
  }
  REPR_END(')');
}


static void repr(RPARAMS, const node_t* nullable n) {
  if (n == NULL) {
    REPR_BEGIN('(', "null");
    REPR_END(')');
    return;
  }

  const char* kindname = STRTAB_GET(nodekind_strtab, n->kind);
  REPR_BEGIN('(', kindname);

  // name up front, even if seen
  if (node_isexpr(n)) {
    if (n->kind == EXPR_FUN && ((fun_t*)n)->name) {
      CHAR(' '), PRINT(((fun_t*)n)->name);
      if (seen(r, n))
        goto end;
      NEWLINE();
      indent += INDENT;
      goto meta; // avoid second call to seen()
    } else if (node_islocal(n)) {
      CHAR(' '), PRINT(((local_t*)n)->name), CHAR(' ');
    } else {
      CHAR(' ');
    }
  }

  if (seen(r, n))
    goto end;

meta:

  // {flags} and <type>
  if (node_isexpr(n)) {
    expr_t* expr = (expr_t*)n;
    if (expr->flags & (EX_RVALUE | EX_OPTIONAL)) {
      CHAR('{');
      if (expr->flags & EX_RVALUE)   CHAR('r');
      if (expr->flags & EX_OPTIONAL) CHAR('o');
      PRINT("} ");
    }
    if (expr->type) {
      repr_type(RARGSFL(fl | REPRFLAG_HEAD), expr->type);
    } else {
      PRINT("<?>");
    }
    if (n->kind == EXPR_FUN && ((fun_t*)n)->name)
      indent -= INDENT;
  }

  switch (n->kind) {

  case NODE_UNIT:    repr_nodearray(RARGS, &((unit_t*)n)->children); break;
  case STMT_TYPEDEF: repr_typedef(RARGS, (typedef_t*)n); break;
  case EXPR_FUN:     repr_fun(RARGS, (fun_t*)n); break;
  case EXPR_CALL:    repr_call(RARGS, (call_t*)n); break;

  case EXPR_RETURN:
    if (((const retexpr_t*)n)->value)
      CHAR(' '), repr(RARGS, (node_t*)((const retexpr_t*)n)->value);
    break;

  case EXPR_BLOCK: {
    const block_t* b = (const block_t*)n;
    repr_nodearray(RARGS, &b->children);
    cleanup(RARGS, &b->cleanup);
    break;
  }

  case EXPR_BOOLLIT:
    CHAR(' '), PRINT(((const boollit_t*)n)->val ? "true" : "false");
    break;

  case EXPR_INTLIT: {
    u64 u = ((intlit_t*)n)->intval;
    CHAR(' ');
    if (!((intlit_t*)n)->type->isunsigned && (u & 0x1000000000000000)) {
      u &= ~0x1000000000000000;
      CHAR('-');
    }
    PRINT("0x");
    buf_print_u64(&r->outbuf, u, 16);
    break;
  }

  case EXPR_FLOATLIT:
    if (((const floatlit_t*)n)->type == type_f64) {
      PRINTF(" %f", ((const floatlit_t*)n)->f64val);
    } else {
      PRINTF(" %f", ((const floatlit_t*)n)->f32val);
    }
    break;

  case EXPR_MEMBER:
    CHAR(' '), PRINT(((const member_t*)n)->name);
    CHAR(' '), repr(RARGS, (const node_t*)((const member_t*)n)->recv);
    break;

  case EXPR_ID:
    CHAR(' '), PRINT(((idexpr_t*)n)->name);
    if (((idexpr_t*)n)->ref) {
      CHAR(' ');
      repr(RARGSFL(fl | REPRFLAG_HEAD), (const node_t*)((idexpr_t*)n)->ref);
    }
    break;

  case EXPR_PREFIXOP:
  case EXPR_POSTFIXOP: {
    unaryop_t* op = (unaryop_t*)n;
    CHAR(' '), PRINT(tok_repr(op->op));
    CHAR(' '), repr(RARGS, (node_t*)op->expr);
    break;
  }

  case EXPR_IF: {
    ifexpr_t* e = (ifexpr_t*)n;
    repr(RARGS, (const node_t*)e->cond);
    repr(RARGS, (const node_t*)e->thenb);
    if (e->elseb)
      repr(RARGS, (const node_t*)e->elseb);
    break;
  }

  case EXPR_FOR: {
    forexpr_t* e = (forexpr_t*)n;
    if (e->start || e->end) {
      REPR_BEGIN('(', "");
      CHAR(' ');
      repr(RARGSFL(fl | REPRFLAG_HEAD), (const node_t*)e->start);
      repr(RARGS, (const node_t*)e->cond);
      repr(RARGS, (const node_t*)e->end);
      REPR_END(')');
    } else {
      repr(RARGS, (const node_t*)e->cond);
    }
    repr(RARGS, (const node_t*)e->body);
    break;
  }

  case EXPR_DEREF:
    CHAR(' '), repr(RARGS, (node_t*)((unaryop_t*)n)->expr);
    break;

  case EXPR_BINOP: {
    binop_t* op = (binop_t*)n;
    CHAR(' '), PRINT(tok_repr(op->op));
    CHAR(' '), repr(RARGS, (node_t*)op->left);
    CHAR(' '), repr(RARGS, (node_t*)op->right);
    break;
  }

  case EXPR_FIELD:
  case EXPR_PARAM:
  case EXPR_LET:
  case EXPR_VAR: {
    const local_t* var = (const local_t*)n;
    CHAR(' '); PRINT(var->name);
    if (type_isowner(var->type))
      PRINT(owner_islive(var) ? " {live}" : " {dead}");
    if (var->init) {
      CHAR(' ');
      repr(RARGS, (const node_t*)var->init);
    }
    break;
  }

  }

end:
  REPR_END(')');
}


err_t node_repr(buf_t* buf, const node_t* n) {
  repr_t r = {
    .outbuf = *buf,
  };
  if (!map_init(&r.seen, buf->ma, 64))
    return ErrNoMem;
  repr(&r, INDENT, REPRFLAG_HEAD, n);
  *buf = r.outbuf;
  map_dispose(&r.seen, buf->ma);
  return r.err;
}


static u32 u64log10(u64 u) {
  // U64_MAX 18446744073709551615
  u32 w = 20;
  u64 x = 10000000000000000000llu;
  while (w > 1) {
    if (u >= x)
      break;
    x /= 10;
    w--;
  }
  return w;
}


srcrange_t node_srcrange(const node_t* n) {
  srcrange_t r = { .start = n->loc, .focus = n->loc };
  switch (n->kind) {
    case EXPR_INTLIT:
      r.end = r.focus;
      r.end.col += u64log10(((intlit_t*)n)->intval); // FIXME e.g. 0xbeef
      break;
    case EXPR_ID:
      r.end = r.focus;
      r.end.col += strlen(((idexpr_t*)n)->name);
      break;
    case EXPR_CALL: {
      const call_t* call = (const call_t*)n;
      if (call->recv)
        r.start = node_srcrange((node_t*)call->recv).start;
      if (call->args.len > 0) {
        r.end = node_srcrange(call->args.v[call->args.len-1]).end;
      } else {
        r.end = r.focus;
      }
      r.end.col++; // ")"
      break;
    }
    default:
      dlog("TODO %s %s", __FUNCTION__, nodekind_name(n->kind));
  }
  return r;
}
