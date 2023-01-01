// diagnostics reporting
// SPDX-License-Identifier: Apache-2.0
#include "colib.h"
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
#define NK_UNKNOWN_STR "NODE_???"
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
  nodekind_t kind = ((const node_t*)n)->kind;
  if (nodekind_isprimtype(kind) || kind == TYPE_UNKNOWN) {
    // atoms/leaves (has no fields)
    return false;
  }
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
  repr_type(RARGS, (type_t*)&n->type);
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
  if (n->recvt) {
    REPR_BEGIN('(', "recvt ");
    repr_type(RARGSFL(fl | REPRFLAG_HEAD), n->recvt);
    REPR_END(')');
  }
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
  for (usize i = 0; i < n->args.len; i++) {
    CHAR(' ');
    repr(RARGS, (const node_t*)n->args.v[i]);
  }
}


static void repr_typecons(RPARAMS, const typecons_t* n) {
  if (n->expr) {
    CHAR(' ');
    repr(RARGS, (const node_t*)n->expr);
  }
}


static void flags(RPARAMS, const node_t* n) {
  // {flags}
  nodeflag_t flags = n->flags;

  // don't include NF_UNKNOWN for TYPE_UNKNOWN (always and obviously true)
  flags &= ~(NF_UNKNOWN * (nodeflag_t)(n->kind == TYPE_UNKNOWN));

  if (flags & (NF_RVALUE | NF_OPTIONAL | NF_UNKNOWN)) {
    PRINT(" {");
    if (flags & NF_RVALUE)   CHAR('r');
    if (flags & NF_OPTIONAL) CHAR('o');
    if (flags & NF_UNKNOWN)  CHAR('u');
    CHAR('}');
  }
}


static void repr_type(RPARAMS, const type_t* t) {
  assert(node_istype((node_t*)t));

  const char* kindname;
  if (t->kind == TYPE_UNKNOWN) {
    kindname = "?";
  } else {
    kindname = STRTAB_GET(nodekind_strtab, t->kind) + strlen("TYPE_");
  }

  REPR_BEGIN('<', kindname);
  bool isnew = !seen(r, t);

  // {flags}
  if (isnew)
    flags(RARGS, (node_t*)t);

  switch (t->kind) {
  case TYPE_STRUCT:
    repr_struct(RARGS, (const structtype_t*)t, isnew);
    break;
  case TYPE_FUN:
    if (isnew)
      repr_funtype(RARGS, (const funtype_t*)t);
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
  case TYPE_ALIAS:
    CHAR(' '), PRINT(((aliastype_t*)t)->name);
    if (isnew) {
      CHAR(' ');
      repr_type(RARGSFL(fl | REPRFLAG_HEAD), ((aliastype_t*)t)->elem);
    }
    break;
  case TYPE_ARRAY:
    dlog("TODO subtype %s", nodekind_name(t->kind));
    break;
  case TYPE_UNRESOLVED:
    CHAR(' '), PRINT(((unresolvedtype_t*)t)->name);
    break;
  }
  REPR_END('>');
}


static void drops(RPARAMS, const droparray_t* drops) {
  if (drops->len == 0)
    return;
  REPR_BEGIN('(', "drops");
  for (u32 i = 0; i < drops->len; i++) {
    drop_t* d = &drops->v[i];
    CHAR(' ');
    PRINT(d->name);
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
  if (nodekind_isexpr(n->kind))
    kindname += strlen("EXPR_");
  REPR_BEGIN('(', kindname);

  bool isnew = !seen(r, n);

  // name up front of functions and variables, even if seen
  if (node_isexpr(n)) {
    if (n->kind == EXPR_FUN && ((fun_t*)n)->name) {
      fun_t* fn = (fun_t*)n;
      CHAR(' ');
      if (fn->recvt) {
        if (fn->recvt->kind == TYPE_STRUCT) {
          PRINT(((structtype_t*)fn->recvt)->name);
        } else if (fn->recvt->kind == TYPE_ALIAS) {
          PRINT(((aliastype_t*)fn->recvt)->name);
        } else {
          repr_type(RARGSFL(fl | REPRFLAG_HEAD), fn->recvt);
        }
        CHAR('.');
      }
      PRINT(fn->name);
      indent += INDENT;
    } else if (node_islocal(n)) {
      CHAR(' '), PRINT(((local_t*)n)->name);
    } else if (n->kind == EXPR_ID) {
      CHAR(' '), PRINT(((idexpr_t*)n)->name);
    } else if (n->kind == EXPR_MEMBER) {
      CHAR(' '), PRINT(((member_t*)n)->name);
    }
    PRINTF(" #%u", n->nrefs);
  } else if (n->kind == TYPE_UNRESOLVED) {
    CHAR(' '), PRINT(((unresolvedtype_t*)n)->name);
  }

  if (!isnew)
    goto end;

  flags(RARGS, n);

  // <type>
  if (node_isexpr(n)) {
    if (r->outbuf.len > 0 && r->outbuf.chars[r->outbuf.len-1] != ' ')
      CHAR(' ');
    expr_t* expr = (expr_t*)n;
    if (expr->type) {
      repr_type(RARGSFL(fl | REPRFLAG_HEAD), expr->type);
    } else {
      PRINT("<?>");
    }
    if (n->kind == EXPR_FUN && ((fun_t*)n)->name)
      indent -= INDENT;
  }

  switch (n->kind) {

  case NODE_UNIT:     repr_nodearray(RARGS, &((unit_t*)n)->children); break;
  case STMT_TYPEDEF:  repr_typedef(RARGS, (typedef_t*)n); break;
  case EXPR_FUN:      repr_fun(RARGS, (fun_t*)n); break;
  case EXPR_CALL:     repr_call(RARGS, (call_t*)n); break;
  case EXPR_TYPECONS: repr_typecons(RARGS, (typecons_t*)n); break;

  case EXPR_RETURN:
    if (((const retexpr_t*)n)->value)
      CHAR(' '), repr(RARGS, (node_t*)((const retexpr_t*)n)->value);
    break;

  case EXPR_BLOCK: {
    const block_t* b = (const block_t*)n;
    repr_nodearray(RARGS, &b->children);
    drops(RARGS, &b->drops);
    break;
  }

  case EXPR_BOOLLIT:
    CHAR(' '), PRINT(((const intlit_t*)n)->intval ? "true" : "false");
    break;

  case EXPR_INTLIT: {
    u64 u = ((intlit_t*)n)->intval;
    CHAR(' ');
    if (!type_isunsigned(((intlit_t*)n)->type) && (u & 0x1000000000000000)) {
      u &= ~0x1000000000000000;
      CHAR('-');
    }
    PRINT("0x");
    buf_print_u64(&r->outbuf, u, 16);
    break;
  }

  case EXPR_FLOATLIT:
    PRINTF(" %f", ((const floatlit_t*)n)->f64val);
    break;

  case EXPR_MEMBER:
    CHAR(' '), repr(RARGS, (node_t*)((member_t*)n)->recv);
    CHAR(' '), repr(RARGS, (node_t*)((member_t*)n)->target);
    break;

  case EXPR_ID:
    if (((idexpr_t*)n)->ref) {
      CHAR(' ');
      repr(RARGSFL(fl | REPRFLAG_HEAD), (const node_t*)((idexpr_t*)n)->ref);
    }
    break;

  case EXPR_PREFIXOP:
  case EXPR_POSTFIXOP: {
    unaryop_t* op = (unaryop_t*)n;
    CHAR(' '), PRINT(op_name(op->op));
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

  case EXPR_ASSIGN:
  case EXPR_BINOP: {
    binop_t* op = (binop_t*)n;
    CHAR(' '), PRINT(op_name(op->op));
    CHAR(' '), repr(RARGS, (node_t*)op->left);
    CHAR(' '), repr(RARGS, (node_t*)op->right);
    break;
  }

  case EXPR_FIELD:
  case EXPR_PARAM:
  case EXPR_LET:
  case EXPR_VAR: {
    const local_t* var = (const local_t*)n;
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


origin_t node_origin(const locmap_t* lm, const node_t* n) {
  origin_t r = origin_make(lm, n->loc);
  switch (n->kind) {

  case EXPR_INTLIT:
    if (r.width == 0)
      r.width = u64log10(((intlit_t*)n)->intval); // FIXME e.g. 0xbeef
    break;

  case EXPR_ID:
    r.width = strlen(((idexpr_t*)n)->name);
    break;

  case TYPE_UNRESOLVED:
    r.width = strlen(((unresolvedtype_t*)n)->name);
    break;

  case EXPR_DEREF:
    return origin_union(r, node_origin(lm, (node_t*)((unaryop_t*)n)->expr));

  case EXPR_LET:
    return origin_make(lm, loc_union(((local_t*)n)->loc, ((local_t*)n)->nameloc));

  case STMT_TYPEDEF: {
    typedef_t* td = (typedef_t*)n;
    if (loc_line(td->type.loc))
      return origin_make(lm, td->type.loc);
    return r;
  }

  case EXPR_BINOP: {
    binop_t* op = (binop_t*)n;
    if (loc_line(op->left->loc) == 0 || loc_line(op->right->loc) == 0)
      return r;
    origin_t left_origin = origin_make(lm, op->left->loc);
    origin_t right_origin = origin_make(lm, op->right->loc);
    r = origin_union(left_origin, right_origin);
    r.focus_col = loc_col(n->loc);
    return r;
  }

  case EXPR_CALL: {
    const call_t* call = (call_t*)n;
    if (call->recv)
      r = origin_union(r, node_origin(lm, (node_t*)call->recv));
    r.width++; // "("
    if (call->args.len > 0)
      r = origin_union(r, node_origin(lm, call->args.v[call->args.len-1]));
    r.width++; // ")"
    break;
  }

  }
  return r;
}
