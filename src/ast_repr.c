// SPDX-License-Identifier: Apache-2.0
#include "colib.h"
#include "compiler.h"


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
  #define _(NAME, ...) NK_##NAME, NK__##NAME = NK_##NAME + strlen(#NAME),
  FOREACH_NODEKIND(_)
  #undef _
  NK_UNKNOWN, NK__UNKNOWN = NK_UNKNOWN + strlen(NK_UNKNOWN_STR),
};
static const struct {
  int  offs[NODEKIND_COUNT + 1]; // index into strs
  char strs[];
} nodekind_strtab = {
  { // get offset from enum
    #define _(NAME, ...) NK_##NAME,
    FOREACH_NODEKIND(_)
    #undef _
    NK_UNKNOWN,
  }, {
    #define _(NAME, ...) #NAME "\0"
    FOREACH_NODEKIND(_)
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

#define PRINTN(cstr, len) ( \
  buf_append(&r->outbuf, (cstr), (len)) ?: seterr(r, ErrNoMem) )

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
  ( CHAR((closech)), indent -= INDENT )


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
  if (nodekind_isprimtype(kind)) {
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
  PRINT("ʹ");
  return true;
}


static void repr_visibility(RPARAMS, const node_t* n) {
  CHAR(' '), PRINT(visibility_str(n->flags));
}


static void repr_nodearray(RPARAMS, const nodearray_t* nodes) {
  for (usize i = 0; i < nodes->len; i++) {
    CHAR(' ');
    repr(RARGS, nodes->v[i]);
  }
}


static void repr_typedef(RPARAMS, const typedef_t* n) {
  repr_visibility(RARGS, (node_t*)n);
  CHAR(' ');
  repr_type(RARGS, n->type);
}


static void repr_struct(RPARAMS, const structtype_t* n, bool isnew) {
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


static void repr_importid(RPARAMS, const importid_t* id) {
  if (id->origname)
    PRINT(id->origname), PRINT(" as ");
  if (id->name == sym__) {
    CHAR('*');
  } else {
    PRINT(id->name ? id->name : "{null}");
  }
}


static void repr_import(RPARAMS, const import_t* im) {
  CHAR('"');
  buf_appendrepr(&r->outbuf, im->path, strlen(im->path));
  CHAR('"');
  if (im->name != sym__)
    PRINT(" as "), PRINT(im->name);
  if (im->idlist) {
    PRINT(" (members");
    for (const importid_t* id = im->idlist; id; id = id->next_id) {
      REPR_BEGIN('(', "IMPORT ");
      repr_importid(RARGS, id);
      REPR_END(')');
    }
    CHAR(')');
  }
  if (im->pkg && im->pkg->api_ns)
    repr(RARGS, (node_t*)im->pkg->api_ns);
}


static void repr_unit(RPARAMS, const unit_t* n) {
  CHAR(' ');
  if (n->srcfile) {
    PRINTN(n->srcfile->name.p, n->srcfile->name.len);
  } else {
    PRINT("<input>");
  }

  if (n->importlist) {
    REPR_BEGIN('(', "import ");
    for (const import_t* im = n->importlist; im; im = im->next_import) {
      REPR_BEGIN('(', "");
      repr_import(RARGS, im);
      REPR_END(')');
    }
    REPR_END(')');
  }

  repr_nodearray(RARGS, &n->children);
}


static void repr_nsexpr(RPARAMS, const nsexpr_t* n) {
  for (usize i = 0; i < n->members.len; i++) {
    REPR_BEGIN('(', n->member_names[i]);
    repr(RARGS, n->members.v[i]);
    REPR_END(')');
  }
}


static void flags(RPARAMS, const node_t* n) {
  // {flags}
  nodeflag_t flags = n->flags;

  // don't include NF_UNKNOWN for TYPE_UNKNOWN (always and obviously true)
  flags &= ~(NF_UNKNOWN * (nodeflag_t)(n->kind == TYPE_UNKNOWN));

  if (flags & ( NF_RVALUE | NF_NEG | NF_UNKNOWN
              | NF_TEMPLATE | NF_TEMPLATEI | NF_CYCLIC))
  {
    PRINT(" {");
    if (flags & NF_RVALUE)    CHAR('r');
    if (flags & NF_NEG)       CHAR('n');
    if (flags & NF_UNKNOWN)   CHAR('u');
    if (flags & NF_TEMPLATE)  CHAR('t');
    if (flags & NF_TEMPLATEI) CHAR('i');
    if (flags & NF_CYCLIC)    CHAR('c');
    CHAR('}');
  }
}


static void repr_type(RPARAMS, const type_t* t) {
  assert(node_istype((node_t*)t));

  const char* kindname;
  if (t->kind == TYPE_UNKNOWN) {
    kindname = "?";
  } else if (t->kind >= NODEKIND_COUNT) {
    kindname = "NODE_???";
  } else {
    kindname = STRTAB_GET(nodekind_strtab, t->kind) + strlen("TYPE_");
  }

  REPR_BEGIN('[', kindname);
  bool isnew = !seen(r, t);

  if (t->kind == TYPE_STRUCT && ((structtype_t*)t)->name)
    CHAR(' '), PRINT(((structtype_t*)t)->name);

  // {flags}
  if (isnew)
    flags(RARGS, (node_t*)t);

  // templateparams
  if (nodekind_isusertype(t->kind) && ((usertype_t*)t)->templateparams.len) {
    CHAR(' ');
    if (isnew) {
      REPR_BEGIN('<', "");
    } else {
      CHAR('<');
    }
    const usertype_t* ut = (usertype_t*)t;
    for (u32 i = 0; i < ut->templateparams.len; i++) {
      if (i) CHAR(' ');
      repr(RARGSFL(fl | REPRFLAG_HEAD), ut->templateparams.v[i]);
    }
    if (isnew) {
      REPR_END('>');
    } else {
      CHAR('>');
    }
  }

  switch (t->kind) {
  case TYPE_STRUCT:
    if (isnew)
      repr_struct(RARGS, (structtype_t*)t, isnew);
    break;
  case TYPE_FUN:
    if (isnew)
      repr_funtype(RARGS, (funtype_t*)t);
    break;
  case TYPE_PTR:
    CHAR(' '), repr_type(RARGSFL(fl | REPRFLAG_HEAD), ((ptrtype_t*)t)->elem);
    break;
  case TYPE_MUTREF:
    PRINT(" mut");
    FALLTHROUGH;
  case TYPE_REF:
    CHAR(' '), repr_type(RARGSFL(fl | REPRFLAG_HEAD), ((reftype_t*)t)->elem);
    break;
  case TYPE_OPTIONAL:
    CHAR(' '), repr_type(RARGSFL(fl | REPRFLAG_HEAD), ((opttype_t*)t)->elem);
    break;
  case TYPE_ALIAS:
    CHAR(' '), PRINT(((aliastype_t*)t)->name);
    if (isnew) {
      CHAR(' '), repr_type(RARGSFL(fl | REPRFLAG_HEAD), ((aliastype_t*)t)->elem);
    }
    break;
  case TYPE_ARRAY:
    if (((arraytype_t*)t)->len > 0) {
      PRINTF(" %llu", ((arraytype_t*)t)->len);
    } else if (((arraytype_t*)t)->lenexpr) {
      CHAR(' ');
      repr(RARGSFL(fl | REPRFLAG_HEAD), (node_t*)((arraytype_t*)t)->lenexpr);
    }
    CHAR(' '), repr_type(RARGSFL(fl | REPRFLAG_HEAD), ((arraytype_t*)t)->elem);
    break;
  case TYPE_MUTSLICE:
    PRINT(" mut");
    FALLTHROUGH;
  case TYPE_SLICE:
    CHAR(' '), repr_type(RARGSFL(fl | REPRFLAG_HEAD), ((slicetype_t*)t)->elem);
    break;
  case TYPE_TEMPLATE: {
    const templatetype_t* tt = (templatetype_t*)t;
    CHAR(' '), repr(
      RARGSFL(fl | REPRFLAG_HEAD), (node_t*)tt->recv);
    for (u32 i = 0; i < tt->args.len; i++)
      repr_type(RARGS, (type_t*)tt->args.v[i]);
    break;
  }
  case TYPE_PLACEHOLDER:
    CHAR(' '), repr(
      RARGSFL(fl | REPRFLAG_HEAD), (node_t*)((placeholdertype_t*)t)->templateparam);
    break;
  case TYPE_UNRESOLVED:
    CHAR(' '), PRINT(((unresolvedtype_t*)t)->name);
    break;
  }
  REPR_END(']');
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
    if ((fl & REPRFLAG_HEAD) == 0)
      NEWLINE();
    PRINT("null");
    return;
  }

  if (node_istype(n))
    return repr_type(RARGS, (type_t*)n);

  const char* kindname = STRTAB_GET(nodekind_strtab, n->kind);
  if (nodekind_isexpr(n->kind)) {
    kindname += strlen("EXPR_");
  } else if (n->kind == NODE_UNIT || n->kind == NODE_TPLPARAM) {
    kindname += strlen("NODE_");
  }
  REPR_BEGIN('(', kindname);

  bool isnew = !seen(r, n);

  // name up front of functions and variables, even if seen
  if (node_isexpr(n)) {
    if (n->kind == EXPR_FUN && ((fun_t*)n)->name) {
      fun_t* fn = (fun_t*)n;
      repr_visibility(RARGS, (node_t*)fn);
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
    } else if (n->kind == EXPR_NS) {
      CHAR(' ');
      const nsexpr_t* ns = (nsexpr_t*)n;
      if ((ns->flags & NF_PKGNS) == 0 && ns->name && ns->name != sym__) {
        PRINT(ns->name);
      } else if ((ns->flags & NF_PKGNS) && ns->pkg != NULL) {
        CHAR('"');
        buf_appendrepr(&r->outbuf, ns->pkg->path.p, ns->pkg->path.len);
        CHAR('"');
      } else {
        PRINTF("%p", ns);
      }
    }
    // PRINTF(" #%u", n->nuse);
  } else if (n->kind == NODE_TPLPARAM) {
    CHAR(' '), PRINT(((templateparam_t*)n)->name);
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

  case STMT_TYPEDEF:  repr_typedef(RARGS, (typedef_t*)n); break;
  case EXPR_FUN:      repr_fun(RARGS, (fun_t*)n); break;
  case EXPR_CALL:     repr_call(RARGS, (call_t*)n); break;
  case EXPR_TYPECONS: repr_typecons(RARGS, (typecons_t*)n); break;
  case EXPR_NS:       repr_nsexpr(RARGS, (nsexpr_t*)n); break;
  case NODE_UNIT:     repr_unit(RARGS, (unit_t*)n); break;

  case EXPR_RETURN:
    if (((retexpr_t*)n)->value)
      CHAR(' '), repr(RARGS, (node_t*)((retexpr_t*)n)->value);
    break;

  case EXPR_BLOCK: {
    const block_t* b = (block_t*)n;
    repr_nodearray(RARGS, &b->children);
    drops(RARGS, &b->drops);
    break;
  }


  case EXPR_BOOLLIT:
    CHAR(' '), PRINT(((intlit_t*)n)->intval ? "true" : "false");
    break;

  case EXPR_INTLIT: {
    u64 u = ((intlit_t*)n)->intval;
    CHAR(' ');
    if (!type_isunsigned(((intlit_t*)n)->type) && (u & 0x8000000000000000)) {
      u = -u;
      CHAR('-');
    }
    buf_print_u64(&r->outbuf, u, 10);
    break;
  }

  case EXPR_FLOATLIT:
    PRINTF(" %f", ((floatlit_t*)n)->f64val);
    break;

  case EXPR_STRLIT:
    CHAR(' ');
    CHAR('"');
    buf_appendrepr(&r->outbuf, ((strlit_t*)n)->bytes, ((strlit_t*)n)->len);
    CHAR('"');
    break;

  case EXPR_ARRAYLIT:
    if (((arraylit_t*)n)->values.len > 0) {
      CHAR(' ');
      repr_nodearray(RARGS, &((arraylit_t*)n)->values);
    }
    break;

  case EXPR_MEMBER:
    CHAR(' '), repr(RARGS, (node_t*)((member_t*)n)->recv);
    REPR_BEGIN('(', "target ");
    repr(RARGSFL(fl | REPRFLAG_HEAD), (node_t*)((member_t*)n)->target);
    REPR_END(')');
    break;

  case NODE_TPLPARAM:
    if (((templateparam_t*)n)->init)
      CHAR(' '), repr(RARGS, ((templateparam_t*)n)->init);
    break;

  case EXPR_SUBSCRIPT: {
    const subscript_t* ss = (const subscript_t*)n;
    if (ss->index->flags & NF_CONST)
      PRINTF(" [%llu]", ss->index_val);
    CHAR(' '), repr(RARGS, (node_t*)ss->index);
    CHAR(' '), repr(RARGS, (node_t*)ss->recv);
    break;
  }

  case EXPR_ID:
    if (((idexpr_t*)n)->ref) {
      CHAR(' ');
      repr(RARGSFL(fl | REPRFLAG_HEAD), (node_t*)((idexpr_t*)n)->ref);
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
    repr(RARGS, (node_t*)e->cond);
    repr(RARGS, (node_t*)e->thenb);
    if (e->elseb)
      repr(RARGS, (node_t*)e->elseb);
    break;
  }

  case EXPR_FOR: {
    forexpr_t* e = (forexpr_t*)n;
    if (e->start || e->end) {
      REPR_BEGIN('(', "");
      CHAR(' ');
      repr(RARGSFL(fl | REPRFLAG_HEAD), (node_t*)e->start);
      repr(RARGS, (node_t*)e->cond);
      repr(RARGS, (node_t*)e->end);
      REPR_END(')');
    } else {
      repr(RARGS, (node_t*)e->cond);
    }
    repr(RARGS, (node_t*)e->body);
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
    const local_t* var = (local_t*)n;
    PRINTF(" {r=%u,w=%d}", var->nuse, var->written);
    if (var->init) {
      CHAR(' ');
      repr(RARGSFL(fl | REPRFLAG_HEAD), (node_t*)var->init);
    }
    break;
  }

  }

end:
  REPR_END(')');
}


static void repr_pkg(repr_t* r, const pkg_t* pkg, const unit_t*const* unitv, u32 unitc) {
  PRINTF("(PKG \"%s\"", pkg->path.p);
  for (u32 i = 0; i < unitc && !r->err; i++)
    repr(r, INDENT, 0, (const node_t*)unitv[i]);
  CHAR(')');
}


err_t ast_repr(buf_t* buf, const node_t* n) {
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


err_t ast_repr_pkg(buf_t* buf, const pkg_t* pkg, const unit_t*const* unitv, u32 unitc) {
  repr_t r = {
    .outbuf = *buf,
  };
  if (!map_init(&r.seen, buf->ma, 64))
    return ErrNoMem;

  repr_pkg(&r, pkg, unitv, unitc);

  *buf = r.outbuf;
  map_dispose(&r.seen, buf->ma);
  return r.err;
}
