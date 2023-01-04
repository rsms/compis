// type-checking pass, which also does late identifier resolution
// SPDX-License-Identifier: Apache-2.0
#include "colib.h"
#include "compiler.h"

#include <stdlib.h> // strtof

#define TRACE_TYPECHECK

#if defined(TRACE_TYPECHECK) && DEBUG
  #define trace(fmt, va...)  \
    _dlog(4, "TC", __FILE__, __LINE__, "%*s" fmt, a->traceindent*2, "", ##va)
  #define tracex(fmt, va...) _dlog(4, "A", __FILE__, __LINE__, fmt, ##va)
#else
  #undef TRACE_TYPECHECK
  #define trace(fmt, va...) ((void)0)
#endif


static const char* fmtnodex(typecheck_t* a, u32 bufindex, const void* nullable n, u32 depth) {
  buf_t* buf = &a->p->tmpbuf[bufindex];
  buf_clear(buf);
  node_fmt(buf, n, depth);
  return buf->chars;
}


static const char* fmtnode(typecheck_t* a, u32 bufindex, const void* nullable n) {
  return fmtnodex(a, bufindex, n, 0);
}


#ifdef TRACE_TYPECHECK
  #define trace_node(a, msg, np) ({ \
    const node_t* __n = *(const node_t**)(np); \
    trace("%s%-14s: %s", (msg), nodekind_name(__n->kind), fmtnode((a), 0, __n)); \
  })

  typedef struct {
    typecheck_t*   a;
    const node_t** np;
    const char*    msg;
  } nodetrace_t;

  static void _trace_cleanup(nodetrace_t* nt) {
    typecheck_t* a = nt->a;
    a->traceindent--;
    type_t* t = NULL;
    const node_t* n = *nt->np;
    if (node_isexpr(n)) {
      t = asexpr(n)->type;
    } else if (node_istype(n)) {
      t = (type_t*)n;
    }
    if (t &&
        ( t == type_unknown ||
          t->kind == TYPE_UNRESOLVED) )
    {
      trace("\e[1;31m%s type not resolved (%s)\e[0m",
        nodekind_name(n->kind), fmtnode(a, 0, n));
    }
    trace("%s%-14s => %s %s", nt->msg, nodekind_name(n->kind),
      t ? nodekind_name(t->kind) : "NULL",
      t ? fmtnode(a, 0, t) : "");
  }

  #define TRACE_NODE(a, msg, np) \
    trace_node((a), (msg), (np)); \
    (a)->traceindent++; \
    nodetrace_t __nt __attribute__((__cleanup__(_trace_cleanup),__unused__)) = \
      {(a), (const node_t**)(np), (msg)};

#else
  #define trace_node(a,msg,n) ((void)0)
  #define TRACE_NODE(a,msg,n) ((void)0)
#endif


#define CHECK_ONCE(node) \
  ( ((node)->flags & NF_CHECKED) == 0 && ((node)->flags |= NF_CHECKED) )


static void incuse(void* node) {
  node_t* n = node;
  n->nuse++;
  if (n->kind == EXPR_ID && ((idexpr_t*)n)->ref)
    incuse(((idexpr_t*)n)->ref);
}

#define use(node) (incuse(node), (node))


static const type_t* unwrap_alias_const(const type_t* t) {
  while (t->kind == TYPE_ALIAS)
    t = assertnotnull(((aliastype_t*)t)->elem);
  return t;
}


// unwrap_id returns node->ref if node is an ID
static node_t* unwrap_id(void* node) {
  node_t* n = node;
  while (n->kind == EXPR_ID)
    n = assertnotnull(((idexpr_t*)n)->ref);
  return n;
}


// unwrap_alias unwraps aliases
// e.g. "MyMyT" => "MyT" => "T"
static type_t* unwrap_alias(type_t* t) {
  while (t->kind == TYPE_ALIAS)
    t = assertnotnull(((aliastype_t*)t)->elem);
  return t;
}


// unwrap_ptr unwraps optional, ref and ptr
// e.g. "?&T" => "&T" => "T"
static type_t* unwrap_ptr(type_t* t) {
  assertnotnull(t);
  for (;;) switch (t->kind) {
    case TYPE_OPTIONAL: t = assertnotnull(((opttype_t*)t)->elem); break;
    case TYPE_REF:      t = assertnotnull(((reftype_t*)t)->elem); break;
    case TYPE_PTR:      t = assertnotnull(((ptrtype_t*)t)->elem); break;
    default:            return t;
  }
}


// unwrap_ptr_and_alias unwraps optional, ref, ptr and alias
// e.g. "?&MyT" => "&MyT" => "MyT" => "T"
static type_t* unwrap_ptr_and_alias(type_t* t) {
  assertnotnull(t);
  for (;;) switch (t->kind) {
    case TYPE_OPTIONAL: t = assertnotnull(((opttype_t*)t)->elem); break;
    case TYPE_REF:      t = assertnotnull(((reftype_t*)t)->elem); break;
    case TYPE_PTR:      t = assertnotnull(((ptrtype_t*)t)->elem); break;
    case TYPE_ALIAS:    t = assertnotnull(((aliastype_t*)t)->elem); break;
    default:            return t;
  }
}


static type_t* concrete_type(const compiler_t* c, type_t* t) {
  for (;;) switch (t->kind) {
  case TYPE_OPTIONAL: t = assertnotnull(((opttype_t*)t)->elem); break;
  case TYPE_ALIAS:    t = assertnotnull(((aliastype_t*)t)->elem); break;
  case TYPE_INT:      t = c->inttype; break;
  case TYPE_UINT:     t = c->uinttype; break;
  default:            return t;
  }
}


bool types_isconvertible(const type_t* dst, const type_t* src) {
  dst = unwrap_alias_const(assertnotnull(dst));
  src = unwrap_alias_const(assertnotnull(src));
  if (dst == src)
    return true;
  if (type_isprim(dst) && type_isprim(src))
    return true;
  return false;
}


bool _types_iscompat(const compiler_t* c, const type_t* dst, const type_t* src) {
  dst = unwrap_alias_const(assertnotnull(dst));
  src = unwrap_alias_const(assertnotnull(src));
  if (dst == src)
    return true;
  switch (dst->kind) {
    case TYPE_INT:
    case TYPE_UINT:
      dst = canonical_primtype(c, dst);
      src = canonical_primtype(c, src);
      FALLTHROUGH;
    case TYPE_I8:
    case TYPE_I16:
    case TYPE_I32:
    case TYPE_I64:
    case TYPE_U8:
    case TYPE_U16:
    case TYPE_U32:
    case TYPE_U64:
      return dst == src;
    case TYPE_PTR:
      // *T <= *T
      // &T <= *T
      return (
        (src->kind == TYPE_PTR || src->kind == TYPE_REF) &&
        types_iscompat(c, ((ptrtype_t*)dst)->elem, ((ptrtype_t*)src)->elem));
    case TYPE_OPTIONAL: {
      // ?T <= T
      // ?T <= ?T
      const opttype_t* d = (opttype_t*)dst;
      if (src->kind == TYPE_OPTIONAL)
        src = ((opttype_t*)src)->elem;
      return types_iscompat(c, d->elem, src);
    }
    case TYPE_REF: {
      // &T    <= &T
      // mut&T <= &T
      // mut&T <= mut&T
      // &T    x= mut&T
      // &T    <= *T
      // mut&T <= *T
      const reftype_t* l = (reftype_t*)dst;
      if (src->kind == TYPE_PTR)
        return types_iscompat(c, l->elem, ((ptrtype_t*)src)->elem);
      const reftype_t* r = (reftype_t*)src;
      return (
        r->kind == TYPE_REF &&
        (r->ismut == l->ismut || r->ismut || !l->ismut) &&
        types_iscompat(c, l->elem, r->elem) );
    }
    case TYPE_SLICE: {
      // &[T]    <= &[T]
      // &[T]    <= mut&[T]
      // mut&[T] <= mut&[T]
      //
      // &[T]    <= &[T N]
      // &[T]    <= mut&[T N]
      // mut&[T] <= mut&[T N]
      const slicetype_t* l = (slicetype_t*)dst;
      switch (src->kind) {
        case TYPE_SLICE: {
          const slicetype_t* r = (slicetype_t*)src;
          return (
            (r->ismut == l->ismut || r->ismut || !l->ismut) &&
            types_iscompat(c, l->elem, r->elem) );
        }
        case TYPE_REF: {
          bool r_ismut = ((reftype_t*)src)->ismut;
          const arraytype_t* r = (arraytype_t*)((reftype_t*)src)->elem;
          return (
            r->kind == TYPE_ARRAY &&
            (r_ismut == l->ismut || r_ismut || !l->ismut) &&
            types_iscompat(c, l->elem, r->elem) );
        }
      }
      return false;
    }
    case TYPE_ARRAY: {
      // [T N] <= [T N]
      const arraytype_t* l = (arraytype_t*)dst;
      const arraytype_t* r = (arraytype_t*)src;
      return r->kind == TYPE_ARRAY &&
             l->len == r->len &&
             types_iscompat(c, l->elem, r->elem);
    }
  }
  return false;
}


bool intern_usertype(compiler_t* c, usertype_t** tp) {
  assert(nodekind_isusertype((*tp)->kind));

  usertype_t** p = (usertype_t**)map_assign_ptr(
    &c->typeidmap, c->ma, typeid((type_t*)*tp));

  if UNLIKELY(!p) {
    report_diag(c, (origin_t){0}, DIAG_ERR, "out of memory (%s)", __FUNCTION__);
    return false;
  }

  if (*p) {
    if (*tp == *p)
      return false;
    //dlog("%s replace %p with existing %p", __FUNCTION__, *tp, *p);
    assert((*p)->kind == (*tp)->kind);
    *tp = *p;
    return true;
  }

  *p = *tp;
  return false;
}


static void seterr(typecheck_t* a, err_t err) {
  if (!a->err)
    a->err = err;
}


static bool noerror(typecheck_t* a) {
  return (!a->err) & (a->compiler->errcount == 0);
}


inline static locmap_t* locmap(typecheck_t* a) {
  return &a->compiler->locmap;
}


// const origin_t to_origin(typecheck_t*, T origin)
// where T is one of: origin_t | loc_t | node_t* (default)
#define to_origin(a, origin) ({ \
  __typeof__(origin)* __tmp = &origin; \
  const origin_t __origin = _Generic(__tmp, \
          origin_t*:  *(origin_t*)__tmp, \
    const origin_t*:  *(origin_t*)__tmp, \
          loc_t*:     origin_make(locmap(a), *(loc_t*)__tmp), \
    const loc_t*:     origin_make(locmap(a), *(loc_t*)__tmp), \
          default:    node_origin(locmap(a), *(node_t**)__tmp) \
  ); \
  __origin; \
})


// void diag(typecheck_t*, T origin, diagkind_t diagkind, const char* fmt, ...)
// where T is one of: origin_t | loc_t | node_t* | expr_t*
#define diag(a, origin, diagkind, fmt, args...) \
  report_diag((a)->compiler, to_origin((a), (origin)), (diagkind), (fmt), ##args)

#define error(a, origin, fmt, args...)    diag(a, origin, DIAG_ERR, (fmt), ##args)
#define warning(a, origin, fmt, args...)  diag(a, origin, DIAG_WARN, (fmt), ##args)
#define help(a, origin, fmt, args...)     diag(a, origin, DIAG_HELP, (fmt), ##args)


static void out_of_mem(typecheck_t* a) {
  error(a, (origin_t){0}, "out of memory");
  seterr(a, ErrNoMem);
}


#define mknode(a, TYPE, kind)  ( (TYPE*)_mknode((a)->p, sizeof(TYPE), (kind)) )


static reftype_t* mkreftype(typecheck_t* a, bool ismut) {
  reftype_t* t = mknode(a, reftype_t, TYPE_REF);
  t->size = a->compiler->ptrsize;
  t->align = t->size;
  t->ismut = ismut;
  return t;
}


static char* mangle(typecheck_t* a, const node_t* n) {
  buf_t* tmpbuf = &a->compiler->diagbuf;
  buf_clear(tmpbuf);
  compiler_mangle(a->compiler, tmpbuf, n);
  char* s = mem_strdup(a->ast_ma, buf_slice(*tmpbuf), 0);
  if UNLIKELY(!s) {
    out_of_mem(a);
    static char last_resort[1] = {0};
    s = last_resort;
  }
  return s;
}


// true if constructing a type t has no side effects
static bool type_cons_no_side_effects(const type_t* t) { switch (t->kind) {
  case TYPE_VOID:
  case TYPE_BOOL:
  case TYPE_I8:
  case TYPE_I16:
  case TYPE_I32:
  case TYPE_I64:
  case TYPE_F32:
  case TYPE_F64:
  case TYPE_INT:
    return true;
  case TYPE_OPTIONAL:
    return type_cons_no_side_effects(((opttype_t*)t)->elem);
  case TYPE_REF:
    return type_cons_no_side_effects(((reftype_t*)t)->elem);
  // TODO: other types. E.g. check fields of struct
  default:
    return false;
}}


bool expr_no_side_effects(const expr_t* n) { switch (n->kind) {
  case EXPR_ID:
  case EXPR_BOOLLIT:
  case EXPR_INTLIT:
  case EXPR_FLOATLIT:
    return true;

  case EXPR_MEMBER:
    return expr_no_side_effects(((member_t*)n)->recv);

  case EXPR_FIELD:
  case EXPR_PARAM:
  case EXPR_LET:
  case EXPR_VAR: {
    const local_t* local = (local_t*)n;
    return (
      type_cons_no_side_effects(local->type) &&
      ( !local->init || expr_no_side_effects(local->init) )
    );
  }

  case EXPR_BINOP:
    return expr_no_side_effects(((binop_t*)n)->right) &&
           expr_no_side_effects(((binop_t*)n)->left);

  case EXPR_PREFIXOP:
  case EXPR_POSTFIXOP: {
    const unaryop_t* op = (unaryop_t*)n;
    if (op->op == OP_INC || op->op == OP_DEC)
      return false;
    return expr_no_side_effects(op->expr);
  }

  // TODO: other kinds
  default:
    return false;
}}


static void error_incompatible_types(
  typecheck_t* a, const void* nullable origin_node, const type_t* x, const type_t* y)
{
  error(a, origin_node, "incompatible types, %s and %s",
    fmtnode(a, 0, x), fmtnode(a, 1, y));
}


static bool check_types_iscompat(
  typecheck_t* a, const void* nullable origin_node,
  const type_t* nullable x, const type_t* nullable y)
{
  if UNLIKELY(!!x * !!y && !types_iscompat(a->compiler, x, y)) {
    // "!!x * !!y": ignore NULL
    error_incompatible_types(a, origin_node, x, y);
    return false;
  }
  return true;
}


static void typectx_push(typecheck_t* a, type_t* t) {
  // if (t->kind == TYPE_UNKNOWN)
  //   t = type_void;
  trace("typectx [%u] %s -> %s",
    a->typectxstack.len, fmtnode(a, 0, a->typectx), fmtnode(a, 1, t));
  if UNLIKELY(!ptrarray_push(&a->typectxstack, a->ma, a->typectx))
    out_of_mem(a);
  a->typectx = t;
}

static void typectx_pop(typecheck_t* a) {
  assert(a->typectxstack.len > 0);
  type_t* t = ptrarray_pop(&a->typectxstack);
  trace("typectx [%u] %s <- %s",
    a->typectxstack.len, fmtnode(a, 1, t), fmtnode(a, 0, a->typectx));
  a->typectx = t;
}


static void enter_scope(typecheck_t* a) {
  if (!scope_push(&a->scope, a->ma))
    out_of_mem(a);
}


static void leave_scope(typecheck_t* a) {
  scope_pop(&a->scope);
}


static void enter_ns(typecheck_t* a, void* node) {
  if UNLIKELY(!ptrarray_push(&a->nspath, a->ma, node))
    out_of_mem(a);
}


static void leave_ns(typecheck_t* a) {
  ptrarray_pop(&a->nspath);
}


static node_t* nullable lookup(typecheck_t* a, sym_t name) {
  node_t* n = scope_lookup(&a->scope, name, U32_MAX);
  if (!n) {
    // look in package scope and its parent universe scope
    void** vp = map_lookup_ptr(&a->p->pkgdefs, name);
    if (!vp)
      return NULL;
    n = *vp;
  }
  return use(n);
}


static void define(typecheck_t* a, sym_t name, void* n) {
  if (name == sym__)
    return;

  trace("define \"%s\" => %s", name, fmtnode(a, 0, n));

  #if DEBUG
    node_t* existing = scope_lookup(&a->scope, name, 0);
    if (existing) {
      error(a, n, "duplicate definition \"%s\"", name);
      if (loc_line(existing->loc))
        warning(a, existing, "previously defined here");
      assertf(0, "duplicate definition \"%s\"", name);
    }
  #endif

  if (!scope_define(&a->scope, a->ma, name, n))
    out_of_mem(a);
}


static void _type(typecheck_t* a, type_t** tp);
static void stmt(typecheck_t* a, stmt_t* n);
static void exprp(typecheck_t* a, expr_t** np);
#define expr(a, n)  exprp(a, (expr_t**)&(n))

inline static void type(typecheck_t* a, type_t** tp) {
  if ( *tp != type_unknown && ((*tp)->flags & NF_CHECKED) == 0 )
    _type(a, tp);
}


static void report_unused(typecheck_t* a, const void* expr_node) {
  assert(node_isexpr(expr_node));
  const expr_t* n = expr_node;

  if (nodekind_islocal(n->kind)) {
    local_t* var = (local_t*)n;
    if (var->name != sym__ && noerror(a))
      warning(a, var->nameloc, "unused %s %s", nodekind_fmt(n->kind), var->name);
    return;
  }

  if UNLIKELY(expr_no_side_effects(n)) {
    if (noerror(a))
      warning(a, n, "unused %s %s", nodekind_fmt(n->kind), fmtnode(a, 0, n));
  }
}


static void block_noscope(typecheck_t* a, block_t* n) {
  TRACE_NODE(a, "", &n);

  if (n->children.len == 0)
    return;

  u32 count = n->children.len;
  u32 stmt_end = count - (u32)(n->flags & NF_RVALUE);

  for (u32 i = 0; i < stmt_end; i++) {
    stmt_t* cn = n->children.v[i];
    stmt(a, cn);

    if (cn->kind == EXPR_RETURN) {
      // mark remaining expressions as unused
      // note: parser reports diagnostics about unreachable code
      for (i++; i < count; i++)
        ((node_t*)n->children.v[i])->nuse = 0;
      stmt_end = count; // avoid rvalue branch later on
      n->type = ((expr_t*)cn)->type;

      if UNLIKELY(!a->fun) {
        error(a, cn, "return outside of function");
      } else if (n->type != type_void) {
        funtype_t* ft = (funtype_t*)a->fun->type;
        if UNLIKELY(ft->result == type_void) {
          error(a, cn, "function %s%sshould not return a value",
            a->fun->name ? a->fun->name : "",
            a->fun->name ? " " : "");
        }
        // note: analysis of fun will check non-void types
      }

      break;
    }
  }

  // report unused expressions
  for (u32 i = 0; i < stmt_end; i++) {
    stmt_t* cn = n->children.v[i];
    if UNLIKELY(cn->nuse == 0 && nodekind_isexpr(cn->kind))
      report_unused(a, cn);
  }

  // if the block is rvalue, treat last entry as implicitly-returned expression
  if (stmt_end < count) {
    assert(n->flags & NF_RVALUE);
    expr_t* lastexpr = n->children.v[stmt_end];
    assert(nodekind_isexpr(lastexpr->kind));
    lastexpr->flags |= NF_RVALUE;
    expr(a, lastexpr);
    lastexpr->nuse = MAX(n->nuse, lastexpr->nuse);
    n->type = lastexpr->type;
  }
}


static void block(typecheck_t* a, block_t* n) {
  enter_scope(a);
  block_noscope(a, n);
  leave_scope(a);
}


static void this_type(typecheck_t* a, local_t* local) {
  type_t* recvt = local->type;
  // pass certain types by value instead of pointer when access is read-only
  if (!local->ismut) {
    if (nodekind_isprimtype(recvt->kind)) // e.g. int, i32
      return;
    if (recvt->kind == TYPE_STRUCT) {
      // small structs
      structtype_t* st = (structtype_t*)recvt;
      u64 maxsize = (u64)a->compiler->ptrsize * 2;
      if ((u32)st->align <= a->compiler->ptrsize && st->size <= maxsize)
        return;
    }
  }
  // pointer type
  reftype_t* t = mkreftype(a, local->ismut);
  t->elem = recvt;
  local->type = (type_t*)t;
}


static void local(typecheck_t* a, local_t* n) {
  assertf(n->nuse == 0 || n->name != sym__, "'_' local that is somehow used");

  type(a, &n->type);

  if (n->init) {
    typectx_push(a, n->type);
    expr(a, n->init);
    typectx_pop(a);

    if (n->type == type_unknown || n->type->kind == TYPE_UNRESOLVED) {
      n->type = n->init->type;
    } else if UNLIKELY(!types_iscompat(a->compiler, n->type, n->init->type)) {
      error(a, n->init, "%s initializer of type %s where type %s is expected",
        nodekind_fmt(n->kind), fmtnode(a, 0, n->init->type), fmtnode(a, 1, n->type));
    }
  }

  if (n->isthis)
    this_type(a, n);

  if UNLIKELY(n->type == type_void || n->type == type_unknown)
    error(a, n, "cannot define %s of type void", nodekind_fmt(n->kind));
}


static void local_var(typecheck_t* a, local_t* n) {
  assert(nodekind_isvar(n->kind));
  local(a, n);
  define(a, n->name, n);
}


static void structtype(typecheck_t* a, structtype_t* st) {
  st->nsparent = a->nspath.v[a->nspath.len - 1];

  u8  align = 0;
  u64 size = 0;

  if (st->name)
    st->mangledname = mangle(a, (node_t*)st);

  enter_ns(a, st);

  for (u32 i = 0; i < st->fields.len; i++) {
    local_t* f = st->fields.v[i];
    local(a, f);
    assertnotnull(f->type);
    if (type_isowner(f->type)) {
      // note: this is optimistic; types aren't marked "DROP" until a
      // custom drop function is implemented, so at this point f->type
      // may be "not owner" since we haven't visited it's drop function yet.
      // e.g.
      //   type A {}
      //   type B { a A }         <—— currently checking B
      //   fun A.drop(mut this){} <—— not yet visited
      // For this reason, we add struct types to a.postanalyze later on.
      st->flags |= NF_SUBOWNERS;
    }
    type_t* t = concrete_type(a->compiler, f->type);
    f->offset = ALIGN2(size, t->align);
    size = f->offset + t->size;
    align = MAX(align, t->align); // alignment of struct is max alignment of fields
  }

  leave_ns(a);

  st->align = align;
  st->size = ALIGN2(size, (u64)align);

  if (!(st->flags & NF_SUBOWNERS)) {
    if UNLIKELY(!map_assign_ptr(&a->postanalyze, a->ma, st))
      out_of_mem(a);
  }
}


static void funtype1(typecheck_t* a, funtype_t** np, type_t* thistype) {
  funtype_t* ft = *np;
  typectx_push(a, thistype);
  for (u32 i = 0; i < ft->params.len; i++)
    local(a, ft->params.v[i]);
  type(a, &ft->result);
  typectx_pop(a);
  // TODO: consider NOT interning function types with parameters that have initializers
  intern_usertype(a->compiler, (usertype_t**)np);
}


static void funtype(typecheck_t* a, funtype_t** np) {
  return funtype1(a, np, type_unknown);
}


// static sym_t nullable nameof(const node_t* n) {
//   switch (n->kind) {
//   case TYPE_STRUCT: return ((structtype_t*)n)->name;
//   case TYPE_ALIAS:  return ((aliastype_t*)n)->name;
//   case EXPR_FUN:    return ((fun_t*)n)->name;
//   default: return NULL;
//   }
// }


static void fun(typecheck_t* a, fun_t* n) {
  fun_t* outer_fun = a->fun;
  a->fun = n;

  if (n->recvt) {
    // type function
    type(a, &n->recvt);
    n->nsparent = (node_t*)n->recvt;
    enter_ns(a, n->recvt);
  } else {
    // plain function
    n->nsparent = a->nspath.v[a->nspath.len - 1];
    if (n->name)
      define(a, n->name, n);
  }

  // check function type first
  if CHECK_ONCE(n->type) {
    type_t* thistype = n->recvt ? n->recvt : type_unknown;
    funtype1(a, (funtype_t**)&n->type, thistype);
  }

  funtype_t* ft = (funtype_t*)n->type;
  assert(ft->kind == TYPE_FUN);

  // parameters
  if (ft->params.len > 0) {
    enter_scope(a);
    for (u32 i = 0; i < ft->params.len; i++) {
      local_t* param = ft->params.v[i];
      if CHECK_ONCE(param) {
        expr(a, param);
      } else if (n->body && param->name != sym__) {
        // Must define in scope, even if we have checked param already.
        // This can happen because multiple functions with the same signatue
        // may share one funtype_t, which holds the parameters.
        define(a, param->name, param);
      }
    }
  }

  // result type
  type(a, &ft->result);

  // mangle name
  n->mangledname = mangle(a, (node_t*)n);

  // check signature of special "drop" function.
  // basically a "poor human's drop trait."
  if (n->recvt && n->name == sym_drop) {
    bool ok = false;
    if (ft->result == type_void && ft->params.len == 1) {
      local_t* param0 = ft->params.v[0];
      ok = param0->type->kind == TYPE_REF && ((reftype_t*)param0->type)->ismut;
      if (ok)
        n->recvt->flags |= NF_DROP;
    }
    if (!ok)
      error(a, n, "invalid signature of \"drop\" function, expecting (mut this)void");
  }

  // body
  if (n->body) {
    n->body->flags |= NF_EXITS;
    if (ft->result != type_void)
      n->body->flags |= NF_RVALUE;
    enter_ns(a, n);
    typectx_push(a, ft->result);
    block(a, n->body);
    typectx_pop(a);
    leave_ns(a);
    n->body->flags &= ~NF_RVALUE;

    // check body type vs function result type
    if UNLIKELY(
      ft->result != type_void &&
      !types_iscompat(a->compiler, ft->result, n->body->type))
    {
      const char* expect = fmtnode(a, 0, ft->result);
      const char* got = fmtnode(a, 1, n->body->type);
      node_t* origin = (node_t*)n->body;
      while (origin->kind == EXPR_BLOCK && ((block_t*)origin)->children.len > 0)
        origin = ((block_t*)origin)->children.v[((block_t*)origin)->children.len-1];
      if (origin) {
        error(a, origin, "unexpected result type %s, function returns %s", got, expect);
      }
    }
  }

  if (n->recvt)
    leave_ns(a);

  if (ft->params.len > 0)
    scope_pop(&a->scope);

  a->fun = outer_fun;
}


static void ifexpr(typecheck_t* a, ifexpr_t* n) {
  // "cond"
  assert(n->cond->flags & NF_RVALUE);
  enter_scope(a);
  use(n->cond);
  expr(a, n->cond);
  if (!type_isbool(n->cond->type) && !type_isopt(n->cond->type))
    return error(a, n->cond, "conditional is not a boolean nor an optional type");

  // "then"
  enter_scope(a);
  n->thenb->flags |= (n->flags & NF_RVALUE); // "then" block is rvalue if "if" is
  block_noscope(a, n->thenb);
  leave_scope(a);

  // "else"
  if (n->elseb) {
    // visit "else" branch
    enter_scope(a);
    n->elseb->flags |= (n->flags & NF_RVALUE); // "else" block is rvalue if "if" is
    block_noscope(a, n->elseb);
    leave_scope(a);
  }

  // leave "cond" scope
  leave_scope(a);

  // if (type_narrowed_binding) {
  //   expr_t* dst = n->cond;
  //   while (dst->kind == EXPR_ID && node_isexpr(((idexpr_t*)dst)->ref))
  //     dst = (expr_t*)((idexpr_t*)dst)->ref;
  //   dst->nuse += type_narrowed_binding->nuse;
  // }

  // unless the "if" is used as an rvalue, we are done
  if ((n->flags & NF_RVALUE) == 0) {
    n->type = type_void;
    return;
  }

  if (n->elseb && n->elseb->type != type_void) {
    // "if ... else" => T
    n->type = n->thenb->type;
    if UNLIKELY(!types_iscompat(a->compiler, n->thenb->type, n->elseb->type)) {
      // TODO: type union
      const char* t1 = fmtnode(a, 0, n->thenb->type);
      const char* t2 = fmtnode(a, 1, n->elseb->type);
      error(a, n->elseb, "incompatible types %s and %s in \"if\" branches", t1, t2);
    }
  } else {
    // "if" => ?T
    n->type = n->thenb->type;
    if (n->type->kind != TYPE_OPTIONAL) {
      opttype_t* t = mknode(a, opttype_t, TYPE_OPTIONAL);
      t->elem = n->type;
      n->type = (type_t*)t;
    }
  }
}


static void idexpr(typecheck_t* a, idexpr_t* n) {
  if (!n->ref) {
    n->ref = lookup(a, n->name);
    if UNLIKELY(!n->ref) {
      error(a, n, "unknown identifier \"%s\"", n->name);
      return;
    }
  }
  expr(a, n->ref);
  if (node_istype(n->ref)) {
    n->type = (type_t*)n->ref;
    type(a, &n->type);
  } else {
    n->type = asexpr(n->ref)->type;
  }
}


static void retexpr(typecheck_t* a, retexpr_t* n) {
  if (n->value) {
    incuse(n->value);
    expr(a, n->value);
    n->type = n->value->type;
  } else {
    n->type = type_void;
  }
}


static bool check_assign_to_member(typecheck_t* a, member_t* m) {
  // check mutability of receiver
  assertnotnull(m->recv->type);
  switch (m->recv->type->kind) {

  case TYPE_STRUCT:
    // assignment to non-ref "this", e.g. "fun Foo.bar(this Foo) { this = Foo() }"
    if UNLIKELY(
      m->recv->kind == EXPR_ID &&
      ((idexpr_t*)m->recv)->ref->kind == EXPR_PARAM &&
      ((local_t*)((idexpr_t*)m->recv)->ref)->isthis)
    {
      error(a, m->recv, "assignment to immutable struct %s", fmtnode(a, 0, m->recv));
      return false;
    }
    return true;

  case TYPE_REF:
    if UNLIKELY(!((reftype_t*)m->recv->type)->ismut) {
      error(a, m->recv, "assignment to immutable reference %s", fmtnode(a, 0, m->recv));
      return false;
    }
    return true;

  default:
    return true;
  }
}


static bool check_assign_to_id(typecheck_t* a, idexpr_t* id) {
  node_t* target = id->ref;
  if (!target) // target is NULL when "id" is undefined
    return false;
  switch (target->kind) {
  case EXPR_ID:
    // this happens when trying to assign to a type-narrowed local
    // e.g. "var a ?int; if a { a = 3 }"
    error(a, id, "cannot assign to type-narrowed binding \"%s\"", id->name);
    return true;
  case EXPR_VAR:
    return true;
  case EXPR_PARAM:
    if (!((local_t*)target)->isthis)
      return true;
    FALLTHROUGH;
  default:
    error(a, id, "cannot assign to %s \"%s\"", nodekind_fmt(target->kind), id->name);
    return false;
  }
}


static bool check_assign(typecheck_t* a, expr_t* target) {
  switch (target->kind) {
  case EXPR_ID:
    return check_assign_to_id(a, (idexpr_t*)target);
  case EXPR_MEMBER:
    return check_assign_to_member(a, (member_t*)target);
  case EXPR_DEREF: {
    // dereference target, e.g. "var x &int ; *x = 3"
    type_t* t = ((unaryop_t*)target)->expr->type;
    if (t->kind != TYPE_REF)
      goto err;
    if UNLIKELY(!((reftype_t*)t)->ismut) {
      const char* s = fmtnode(a, 0, t);
      error(a, target, "cannot assign via immutable reference of type %s", s);
      return false;
    }
    return true;
  }
  }
err:
  error(a, target, "cannot assign to %s", nodekind_fmt(target->kind));
  return false;
}


static void binop1(typecheck_t* a, binop_t* n, bool isassign) {
  if (!isassign) {
    // note: being assign to doesn't count as being used
    use(n->left);
  }
  expr(a, n->left);

  typectx_push(a, n->left->type);
  use(n->right);
  expr(a, n->right);
  typectx_pop(a);

  switch (n->op) {
    case OP_EQ:
    case OP_NEQ:
    case OP_LT:
    case OP_GT:
    case OP_LTEQ:
    case OP_GTEQ:
      n->type = type_bool;
      break;
    default:
      n->type = n->left->type;
  }

  check_types_iscompat(a, n, n->left->type, n->right->type);
}


static void binop(typecheck_t* a, binop_t* n) {
  binop1(a, n, false);
}


static void assign(typecheck_t* a, binop_t* n) {
  binop1(a, n, true);
  check_assign(a, n->left);
}


static void unaryop(typecheck_t* a, unaryop_t* n) {
  expr(a, n->expr);
  n->type = n->expr->type;
  if (n->op == TPLUSPLUS || n->op == TMINUSMINUS) {
    // TODO: specialized check here since it's not actually assignment (ownership et al)
    check_assign(a, n->expr);
  }
}


static void deref(typecheck_t* a, unaryop_t* n) {
  expr(a, n->expr);

  ptrtype_t* t = (ptrtype_t*)n->expr->type;
  if UNLIKELY(t->kind != TYPE_REF && t->kind != TYPE_PTR) {
    error(a, n, "dereferencing non-reference value of type %s", fmtnode(a, 0, t));
  } else {
    n->type = t->elem;
  }
}


static void floatlit(typecheck_t* a, floatlit_t* n) {
  if (a->typectx == type_f32) {
    n->type = type_f32;
    // FIXME: better way to check f32 value (than via sprintf & strtof)
    buf_t* buf = &a->p->tmpbuf[0];
    buf->len = 0;
    if UNLIKELY(!buf_printf(buf, "%g", n->f64val))
      out_of_mem(a);
    float f = strtof(buf->chars, NULL);
    if UNLIKELY(f == HUGE_VALF) {
      // e.g. 1.e39
      error(a, n, "32-bit floating-point constant too large");
      n->f64val = 0.0;
    }
  } else {
    n->type = type_f64;
    if UNLIKELY(n->f64val == HUGE_VAL) {
      // e.g. 1.e309
      error(a, n, "64-bit floating-point constant too large");
      n->f64val = 0.0;
    }
  }
}


static void intlit(typecheck_t* a, intlit_t* n) {
  if (n->type != type_unknown)
    return;

  u64 isneg = 0; // TODO

  type_t* type = a->typectx;
  type_t* basetype = unwrap_alias(type);

  u64 maxval;
  u64 uintval = n->intval;
  if (isneg)
    uintval &= ~0x1000000000000000; // clear negative bit

again:
  switch (basetype->kind) {
  case TYPE_I8:   maxval = 0x7fllu+isneg; break;
  case TYPE_I16:  maxval = 0x7fffllu+isneg; break;
  case TYPE_I32:  maxval = 0x7fffffffllu+isneg; break;
  case TYPE_I64:  maxval = 0x7fffffffffffffffllu+isneg; break;
  case TYPE_U8:   maxval = 0xffllu; break;
  case TYPE_U16:  maxval = 0xffffllu; break;
  case TYPE_U32:  maxval = 0xffffffffllu; break;
  case TYPE_U64:  maxval = 0xffffffffffffffffllu; break;
  case TYPE_INT:  basetype = a->compiler->inttype; goto again;
  case TYPE_UINT: basetype = a->compiler->uinttype; goto again;
  default:
    // all other type contexts results in int, uint, i64 or u64 (depending on value)
    if (a->compiler->intsize == 8) {
      if (isneg) {
        type = type_int;
        maxval = 0x8000000000000000llu;
      } else if (n->intval < 0x8000000000000000llu) {
        n->type = type_int;
        return;
      } else {
        type = type_u64;
        maxval = 0xffffffffffffffffllu;
      }
    } else {
      assertf(a->compiler->intsize >= 4 && a->compiler->intsize < 8,
        "intsize %u not yet supported", a->compiler->intsize);
      if (isneg) {
        if (uintval <= 0x80000000llu)         { n->type = type_int; return; }
        if (uintval <= 0x8000000000000000llu) { n->type = type_i64; return; }
        // too large; trigger error report
        maxval = 0x8000000000000000llu;
        type = type_i64;
      } else {
        if (n->intval <= 0x7fffffffllu)         { n->type = type_int; return; }
        if (n->intval <= 0xffffffffllu)         { n->type = type_uint; return; }
        if (n->intval <= 0x7fffffffffffffffllu) { n->type = type_i64; return; }
        maxval = 0xffffffffffffffffllu;
        type = type_u64;
      }
    }
  }

  if UNLIKELY(uintval > maxval) {
    const char* ts = fmtnode(a, 0, type);
    error(a, n, "integer constant overflows %s", ts);
  }

  n->type = type;
}


static void strlit(typecheck_t* a, strlit_t* n) {
  if (a->typectx == (type_t*)&a->compiler->strtype) {
    n->type = a->typectx;
    return;
  }

  arraytype_t* at = mknode(a, arraytype_t, TYPE_ARRAY);
  at->flags = NF_CHECKED;
  at->elem = type_u8;
  at->len = n->len;
  at->size = at->elem->size * at->len;
  at->align = at->elem->align;

  reftype_t* t = mknode(a, reftype_t, TYPE_REF);
  t->elem = (type_t*)at;

  n->type = (type_t*)t;
}


static expr_t* nullable find_member(typecheck_t* a, type_t* t, sym_t name) {
  type_t* bt = unwrap_ptr_and_alias(t); // e.g. "?&MyMyT" => "T"

  // start with fields for struct
  if (bt->kind == TYPE_STRUCT) {
    structtype_t* st = (structtype_t*)bt;
    for (u32 i = 0; i < st->fields.len; i++) {
      if (((local_t*)st->fields.v[i])->name == name) {
        exprp(a, (expr_t**)&st->fields.v[i]);
        return st->fields.v[i];
      }
    }
  }

  // look for type function, testing each alias in turn, e.g.
  //   1 MyMyT (alias of MyT)
  //   2 MyT (alias of T)
  //   3 T
  bt = unwrap_ptr(t); // e.g. "?*MyMyT" => "MyMyT"
  map_t recvtmap = a->p->recvtmap; // {type_t* => map_t*}
  for (;;) {
    // dlog("get recvtmap for %s %s", nodekind_name(bt->kind), fmtnode(a, 0, bt));
    map_t** mp = (map_t**)map_lookup_ptr(&recvtmap, bt);
    if (mp) {
      assertnotnull(*mp); // {sym_t name => fun_t*}
      fun_t** fnp = (fun_t**)map_lookup_ptr(*mp, name);
      if (fnp) {
        assert((*fnp)->kind == EXPR_FUN);
        if CHECK_ONCE(*fnp)
          fun(a, *fnp);
        return (expr_t*)*fnp;
      }
    }
    if (bt->kind != TYPE_ALIAS)
      break;
    bt = assertnotnull(((aliastype_t*)bt)->elem);
  }

  return NULL;
}


static void member(typecheck_t* a, member_t* n) {
  incuse(n->recv);
  expr(a, n->recv);

  // get receiver type without ref or optional
  type_t* recvt = n->recv->type;

  // resolve target
  typectx_push(a, type_unknown);
  expr_t* target = find_member(a, recvt, n->name);
  typectx_pop(a);

  if (target) {
    n->target = use(target);
    n->type = target->type;
  } else {
    n->type = a->typectx; // avoid cascading errors
    error(a, n, "%s has no field or method \"%s\"", fmtnode(a, 0, recvt), n->name);
  }
}


static void finalize_typecons(typecheck_t* a, typecons_t** np) {
  type_t* t = (*np)->type;

  if (!type_isprim(unwrap_alias(t)))
    return;

  expr_t* expr = (*np)->expr;
  if (!expr)
    return;

  if (types_iscompat(a->compiler, t, expr->type)) {
    // eliminate type cast to equivalent type, e.g. "i8(3)" => "3"
    expr->nuse += MAX(1, (*np)->nuse) - 1;
    *(expr_t**)np = expr;
    return;
  }

  if UNLIKELY(!types_isconvertible(t, expr->type)) {
    const char* dst_s = fmtnode(a, 0, t);
    const char* src_s = fmtnode(a, 1, expr->type);
    error(a, *np, "cannot convert value of type %s to type %s", src_s, dst_s);
    return;
  }
}


static void typecons(typecheck_t* a, typecons_t** np) {
  typecons_t* n = *np;
  if (n->expr) {
    incuse(n->expr);
    typectx_push(a, n->type);
    expr(a, n->expr);
    typectx_pop(a);
  }
  return finalize_typecons(a, np);
}


// —————————————————————————————————————————————————————————————————————————————————
// call


static void error_field_type(typecheck_t* a, const expr_t* arg, const local_t* f) {
  const char* got = fmtnode(a, 0, arg->type);
  const char* expect = fmtnode(a, 1, f->type);
  const void* origin = arg;
  if (arg->kind == EXPR_PARAM)
    origin = assertnotnull(((local_t*)arg)->init);
  error(a, origin, "passing value of type %s for field \"%s\" of type %s",
    got, f->name, expect);
}


static void convert_call_to_typecons(typecheck_t* a, call_t** np, type_t* t) {
  static_assert(sizeof(typecons_t) <= sizeof(call_t), "");

  ptrarray_t args = (*np)->args;
  typecons_t* tc = (typecons_t*)*np;

  tc->kind = EXPR_TYPECONS;
  tc->type = t;
  if (type_isprim(unwrap_alias(t))) {
    assert(args.len == 1);
    tc->expr = args.v[0];
  } else {
    tc->args = args;
  }

  return finalize_typecons(a, (typecons_t**)np);
}


static void check_call_type_struct(typecheck_t* a, call_t* call, structtype_t* t){
  assert(call->args.len <= t->fields.len); // checked by validate_typecall_args

  u32 i = 0;
  ptrarray_t args = call->args;

  // build field map
  map_t fieldmap = a->p->tmpmap;
  map_clear(&fieldmap);
  if UNLIKELY(!map_reserve(&fieldmap, a->ma, t->fields.len))
    return out_of_mem(a);
  for (u32 i = 0; i < t->fields.len; i++) {
    const local_t* f = t->fields.v[i];
    void** vp = map_assign_ptr(&fieldmap, a->ma, f->name);
    assertnotnull(vp); // map_reserve
    *vp = (void*)f;
  }

  // map arguments
  for (; i < args.len; i++) {
    expr_t* arg = args.v[i];
    sym_t name = NULL;

    switch (arg->kind) {
    case EXPR_PARAM:
      name = ((local_t*)arg)->name;
      break;
    case EXPR_ID:
      name = ((idexpr_t*)arg)->name;
      break;
    default:
      error(a, arg,
        "positional argument in struct constructor; use either name:value"
        " or an identifier with the same name as the intended struct field");
      continue;
    }

    // lookup field
    void** vp = map_lookup_ptr(&fieldmap, name);
    if UNLIKELY(!vp || ((node_t*)*vp)->kind != EXPR_FIELD) {
      const char* s = fmtnode(a, 0, t);
      if (!vp) {
        error(a, arg, "no \"%s\" field in struct %s", name, s);
      } else {
        error(a, arg, "duplicate value for field \"%s\" of struct %s", name, s);
        warning(a, *vp, "value for field \"%s\" already provided here", name);
      }
      continue;
    }

    local_t* f = *vp; // load field
    *vp = arg; // mark field name as defined, used for detecting duplicate args
    arg->flags |= NF_RVALUE;

    typectx_push(a, f->type);

    if (arg->kind == EXPR_PARAM) {
      local_t* namedarg = (local_t*)arg;
      assertnotnull(namedarg->init); // checked by parser
      expr(a, namedarg->init);
      namedarg->type = namedarg->init->type;
    } else {
      assert(arg->kind == EXPR_ID); // for future dumb me
      idexpr(a, (idexpr_t*)arg);
    }

    typectx_pop(a);

    if UNLIKELY(!types_iscompat(a->compiler, f->type, arg->type))
      error_field_type(a, arg, f);
  }

  a->p->tmpmap = fieldmap; // in case map grew
}


static void call_type_prim(typecheck_t* a, call_t** np, type_t* dst) {
  call_t* call = *np;
  assert(call->args.len == 1);
  expr_t* arg = call->args.v[0];

  if UNLIKELY(!nodekind_isexpr(arg->kind))
    return error(a, arg, "invalid value");

  if UNLIKELY(arg->kind == EXPR_PARAM) {
    return error(a, arg, "%s type cast does not accept named arguments",
      fmtnode(a, 0, dst));
  }

  typectx_push(a, dst);
  expr(a, arg);
  typectx_pop(a);

  call->type = dst;

  return convert_call_to_typecons(a, np, dst);
}


static void error_call_type_arity(
  typecheck_t* a, call_t* call, type_t* t, u32 minargs, u32 maxargs)
{
  assert(minargs > call->args.len || call->args.len > maxargs);
  const char* typstr = fmtnode(a, 1, t);

  const char* logical_op = "type cast";
  type_t* basetype = unwrap_alias(t);
  if (basetype->kind == TYPE_STRUCT || basetype->kind == TYPE_ARRAY)
    logical_op = "type constructor";

  if (call->args.len < minargs) {
    const void* origin = call->recv;
    if (call->args.len > 0)
      origin = call->args.v[call->args.len - 1];
    error(a, origin, "not enough arguments for %s %s, expecting%s %u",
      typstr, logical_op, minargs != maxargs ? " at least" : "", minargs);
    return;
  }

  const node_t* arg = call->args.v[maxargs];
  const char* argstr = fmtnode(a, 0, arg);
  if (maxargs == 0) {
    // e.g. "void(x)"
    error(a, arg, "unexpected value %s; %s %s accepts no arguments",
      argstr, typstr, logical_op);
  } else {
    error(a, arg, "unexpected extra value %s in %s %s",
      argstr, typstr, logical_op);
  }
}


static bool check_call_type_arity(
  typecheck_t* a, call_t* call, type_t* t, u32 minargs, u32 maxargs)
{
  if UNLIKELY(minargs > call->args.len || call->args.len > maxargs) {
    error_call_type_arity(a, call, t, minargs, maxargs);
    return false;
  }
  return true;
}


static void call_type(typecheck_t* a, call_t** np, type_t* t) {
  call_t* call = *np;
  call->type = t;

  // unwrap alias
  type_t* origt = t; // original type
  t = unwrap_alias(t);

  switch (t->kind) {
  case TYPE_VOID: {
    // no arguments
    if UNLIKELY(!check_call_type_arity(a, call, origt, 0, 0))
      return;
    // convert to typecons
    typecons_t* tc = (typecons_t*)*np;
    tc->kind = EXPR_TYPECONS;
    tc->type = origt;
    tc->expr = NULL;
    return;
  }

  case TYPE_BOOL:
  case TYPE_INT:
  case TYPE_I8:
  case TYPE_I16:
  case TYPE_I32:
  case TYPE_I64:
  case TYPE_F32:
  case TYPE_F64:
    if UNLIKELY(!check_call_type_arity(a, call, origt, 1, 1))
      return;
    return call_type_prim(a, np, origt);

  case TYPE_STRUCT: {
    u32 maxargs = ((structtype_t*)t)->fields.len;
    if UNLIKELY(!check_call_type_arity(a, call, origt, 0, maxargs))
      return;
    return check_call_type_struct(a, call, (structtype_t*)t);
  }

  // TODO
  case TYPE_ARRAY:
    if UNLIKELY(!check_call_type_arity(a, call, origt, 1, U32_MAX))
      return;
    FALLTHROUGH;
  case TYPE_FUN:
  case TYPE_PTR:
  case TYPE_REF:
  case TYPE_OPTIONAL:
    trace("TODO IMPLEMENT %s", nodekind_name(t->kind));
    error(a, call->recv, "NOT IMPLEMENTED: %s", nodekind_name(t->kind));
    return;

  case TYPE_UNRESOLVED:
    // this only happens when there was a type error
    assert(a->compiler->errcount > 0);
    return;

  default:
    assertf(0,"unexpected %s", nodekind_name(t->kind));
    return;
  }
}


static void call_fun(typecheck_t* a, call_t* call, funtype_t* ft) {
  call->type = ft->result;

  u32 paramsc = ft->params.len;
  local_t** paramsv = (local_t**)ft->params.v;
  if (paramsc > 0 && paramsv[0]->isthis) {
    paramsv++;
    paramsc--;
  }

  if UNLIKELY(call->args.len != paramsc) {
    error(a, call, "%s arguments in function call, expected %u",
      call->args.len < paramsc ? "not enough" : "too many", paramsc);
    return;
  }

  bool seen_named_arg = false;

  for (u32 i = 0; i < paramsc; i++) {
    expr_t* arg = call->args.v[i];
    local_t* param = paramsv[i];

    typectx_push(a, param->type);

    if (arg->kind == EXPR_PARAM) {
      // named argument
      local_t* namedarg = (local_t*)arg;
      assertnotnull(namedarg->init); // checked by parser
      expr(a, namedarg->init);
      arg->type = namedarg->init->type;
      seen_named_arg = true;

      if UNLIKELY(namedarg->name != param->name) {
        u32 j = i;
        for (j = 0; j < paramsc; j++) {
          if (paramsv[j]->name == namedarg->name)
            break;
        }
        const char* condition = (j == paramsc) ? "unknown" : "invalid position of";
        error(a, arg, "%s named argument \"%s\", in function call %s", condition,
          namedarg->name, fmtnode(a, 0, ft));
      }
    } else {
      // positional argument
      if UNLIKELY(seen_named_arg) {
        error(a, arg, "positional argument after named argument(s)");
        typectx_pop(a);
        break;
      }
      expr(a, arg);
    }

    typectx_pop(a);

    // check type
    if UNLIKELY(!types_iscompat(a->compiler, param->type, arg->type)) {
      const char* got = fmtnode(a, 0, arg->type);
      const char* expect = fmtnode(a, 1, param->type);
      error(a, arg, "passing value of type %s to parameter of type %s", got, expect);
    }
  }

  if ((call->flags & NF_RVALUE) == 0 && type_isowner(call->type) && noerror(a)) {
    // return value is owner, but it is not used (call is not rvalue)
    warning(a, call, "unused result; ownership transferred from function call");
  }
}


static void call(typecheck_t* a, call_t** np) {
  call_t* n = *np;
  expr(a, n->recv);

  node_t* recv = unwrap_id(n->recv);
  type_t* recvtype;

  if LIKELY(node_isexpr(recv)) {
    recvtype = ((expr_t*)recv)->type;
    if (recvtype->kind == TYPE_FUN)
      return call_fun(a, n, (funtype_t*)recvtype);
  } else if LIKELY(node_istype(recv)) {
    recvtype = (type_t*)recv;
    return call_type(a, np, recvtype);
  }

  // error: bad recv
  n->type = a->typectx; // avoid cascading errors
  if (node_isexpr(recv)) {
    error(a, n->recv, "calling an expression of type %s, expected function or type",
      fmtnode(a, 0, ((expr_t*)recv)->type));
  } else {
    error(a, n->recv, "calling %s; expected function or type", fmtnode(a, 0, recv));
  }
}


static void unresolvedtype(typecheck_t* a, unresolvedtype_t** tp) {
  if ((*tp)->resolved) {
    *(type_t**)tp = (*tp)->resolved;
    return;
  }

  sym_t name = (*tp)->name;
  type_t* t = (type_t*)lookup(a, name);

  // dlog("resolve type \"%s\" => %s %s",
  //   name, nodekind_name(t ? t->kind : 0), t ? fmtnode(a, 0, t) : "(null)");

  if LIKELY(t && nodekind_istype(t->kind)) {
    type(a, &t);
    t->nuse += (*tp)->nuse;
    (*tp)->resolved = t;
    *(type_t**)tp = t;
    return;
  }

  // some error beyond this point

  // not found
  if (!t) {
    error(a, *tp, "unknown type \"%s\"", name);
    return;
  }

  // not a type
  error(a, *tp, "%s is not a type (it's a %s)", name, nodekind_fmt(t->kind));
  if (loc_line(t->loc))
    help(a, t, "%s defined here", name);

  // redefine as "void" in current scope to minimize repetitive errors
  if (!scope_define(&a->scope, a->ma, name, *tp))
    out_of_mem(a);
}


static void typedef_(typecheck_t* a, typedef_t* n) {
  type_t* t = &n->type;
  type(a, &t);
}


static void aliastype(typecheck_t* a, aliastype_t** tp) {
  aliastype_t* t = *tp;
  type(a, &t->elem);
  if UNLIKELY(t->elem == type_void)
    return error(a, t, "cannot alias type void");
}


// static void unknowntype(typecheck_t* a, type_t** tp) {
//   assertf(a->typectx != type_unknown, "unknown type inside unresolved typectx");
//   *tp = a->typectx;
// }


// end call
// —————————————————————————————————————————————————————————————————————————————————


static void _type(typecheck_t* a, type_t** tp) {
  type_t* t = *tp;

  if (t->flags & NF_CHECKED)
    return;
  t->flags |= NF_CHECKED;

  TRACE_NODE(a, "", tp);
  switch ((*tp)->kind) {
    case TYPE_UNRESOLVED: return unresolvedtype(a, (unresolvedtype_t**)tp);
    case TYPE_ALIAS:      return aliastype(a, (aliastype_t**)tp);
    case TYPE_FUN:        return funtype(a, (funtype_t**)tp);
    case TYPE_STRUCT:     return structtype(a, (structtype_t*)*tp);
    case TYPE_REF:        return type(a, &((reftype_t*)(*tp))->elem);
    case TYPE_PTR:        return type(a, &((ptrtype_t*)(*tp))->elem);
    case TYPE_OPTIONAL:   return type(a, &((opttype_t*)(*tp))->elem);
  }
  dlog("TODO %s %s", __FUNCTION__, nodekind_name((*tp)->kind));
}


static void stmt(typecheck_t* a, stmt_t* n) {
  if (n->kind == STMT_TYPEDEF) {
    if (n->flags & NF_CHECKED)
      return;
    n->flags |= NF_CHECKED;
    TRACE_NODE(a, "", &n);
    return typedef_(a, (typedef_t*)n);
  }
  assertf(node_isexpr((node_t*)n), "unexpected node %s", nodekind_name(n->kind));
  return expr(a, n);
}


static void exprp(typecheck_t* a, expr_t** np) {
  expr_t* n = *np;
  if (n->flags & NF_CHECKED)
    return;
  n->flags |= NF_CHECKED;

  TRACE_NODE(a, "", np);

  type(a, &n->type);

  switch ((enum nodekind)n->kind) {
  case EXPR_FUN:       return fun(a, (fun_t*)n);
  case EXPR_IF:        return ifexpr(a, (ifexpr_t*)n);
  case EXPR_ID:        return idexpr(a, (idexpr_t*)n);
  case EXPR_RETURN:    return retexpr(a, (retexpr_t*)n);
  case EXPR_BINOP:     return binop(a, (binop_t*)n);
  case EXPR_ASSIGN:    return assign(a, (binop_t*)n);
  case EXPR_BLOCK:     return block(a, (block_t*)n);
  case EXPR_CALL:      return call(a, (call_t**)np);
  case EXPR_TYPECONS:  return typecons(a, (typecons_t**)np);
  case EXPR_MEMBER:    return member(a, (member_t*)n);
  case EXPR_DEREF:     return deref(a, (unaryop_t*)n);
  case EXPR_INTLIT:    return intlit(a, (intlit_t*)n);
  case EXPR_FLOATLIT:  return floatlit(a, (floatlit_t*)n);
  case EXPR_STRLIT:    return strlit(a, (strlit_t*)n);

  case EXPR_PREFIXOP:
  case EXPR_POSTFIXOP:
    return unaryop(a, (unaryop_t*)n);

  case EXPR_FIELD:
  case EXPR_PARAM:
    return local(a, (local_t*)n);
  case EXPR_VAR:
  case EXPR_LET:
    return local_var(a, (local_t*)n);

  // TODO
  case EXPR_FOR:
    panic("TODO %s", nodekind_name(n->kind));
    break;

  // We should never see these kinds of nodes
  case NODEKIND_COUNT:
  case NODE_BAD:
  case NODE_COMMENT:
  case NODE_UNIT:
  case STMT_TYPEDEF:
  case EXPR_BOOLLIT:
  case TYPE_VOID:
  case TYPE_BOOL:
  case TYPE_I8:
  case TYPE_I16:
  case TYPE_I32:
  case TYPE_I64:
  case TYPE_INT:
  case TYPE_U8:
  case TYPE_U16:
  case TYPE_U32:
  case TYPE_U64:
  case TYPE_UINT:
  case TYPE_F32:
  case TYPE_F64:
  case TYPE_ARRAY:
  case TYPE_SLICE:
  case TYPE_FUN:
  case TYPE_PTR:
  case TYPE_REF:
  case TYPE_OPTIONAL:
  case TYPE_STRUCT:
  case TYPE_ALIAS:
  case TYPE_UNKNOWN:
  case TYPE_UNRESOLVED:
    break;
  }
  assertf(0, "unexpected node %s", nodekind_name(n->kind));
}


static void postanalyze_any(typecheck_t* a, node_t* n);


static void postanalyze_dependency(typecheck_t* a, void* np) {
  node_t* n = np;
  if (n->kind != TYPE_STRUCT)
    return;
  void** vp = map_assign_ptr(&a->postanalyze, a->ma, n);
  if UNLIKELY(!vp)
    return out_of_mem(a);
  if (*vp == (void*)1)
    return;
  postanalyze_any(a, n);
}


static void postanalyze_structtype(typecheck_t* a, structtype_t* st) {
  for (u32 i = 0; i < st->fields.len; i++) {
    local_t* f = st->fields.v[i];
    postanalyze_dependency(a, f->type);
    if (type_isowner(f->type))
      st->flags |= NF_SUBOWNERS;
  }
}


static void postanalyze_any(typecheck_t* a, node_t* n) {
  trace("postanalyze %s %s", nodekind_name(n->kind), fmtnode(a, 0, n));
  switch (n->kind) {
  case TYPE_STRUCT: return postanalyze_structtype(a, (structtype_t*)n);
  case TYPE_ALIAS:  return postanalyze_any(a, (node_t*)((aliastype_t*)n)->elem);
  }
}


static void postanalyze(typecheck_t* a) {
  // Keep going until map only has null entries.
  // postanalyze_any may cause additions to the map.
again:
  mapent_t* e = map_it_mut(&a->postanalyze);
  while (map_itnext_mut(&a->postanalyze, &e)) {
    if (e->value != (void*)1) {
      e->value = (void*)1;
      postanalyze_any(a, (node_t*)e->key);
      goto again;
    }
  }
}


err_t typecheck(parser_t* p, unit_t* unit) {
  scope_clear(&p->scope);

  typecheck_t a = {
    .compiler = p->scanner.compiler,
    .p = p,
    .ma = p->scanner.compiler->ma,
    .ast_ma = p->ast_ma,
    .scope = p->scope,
    .typectx = type_void,
  };

  if (!map_init(&a.postanalyze, a.ma, 32))
    return ErrNoMem;

  enter_scope(&a);
  enter_ns(&a, unit);

  for (u32 i = 0; i < unit->children.len; i++)
    stmt(&a, unit->children.v[i]);

  leave_ns(&a);
  leave_scope(&a);

  postanalyze(&a);

  ptrarray_dispose(&a.nspath, a.ma);
  ptrarray_dispose(&a.typectxstack, a.ma);
  map_dispose(&a.postanalyze, a.ma);

  // update borrowed containers owned by p (in case they grew)
  p->scope = a.scope;

  return a.err;
}
