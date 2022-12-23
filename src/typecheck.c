// type-checking pass, which also does late identifier resolution
// SPDX-License-Identifier: Apache-2.0
#include "c0lib.h"
#include "compiler.h"

#define TRACE_TYPECHECK

#ifdef TRACE_TYPECHECK
  #define trace(fmt, va...)  \
    _dlog(4, "TC", __FILE__, __LINE__, "%*s" fmt, a->traceindent*2, "", ##va)
  #define tracex(fmt, va...) _dlog(4, "A", __FILE__, __LINE__, fmt, ##va)
#else
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
    type_t* effective_type = NULL;
    const node_t* n = *nt->np;
    if (node_isexpr(n)) {
      effective_type = asexpr(n)->type;
    } else if (node_istype(n)) {
      effective_type = (type_t*)n;
    }
    if (effective_type == type_unknown) {
      trace("\e[1;31m%s type not resolved (%s)\e[0m",
        nodekind_name(n->kind), fmtnode(a, 0, n));
    }
    trace("%s%-14s => %s", nt->msg, nodekind_name(n->kind),
      effective_type ? fmtnode(a, 0, effective_type) : "NULL");
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


inline static const type_t* unwind_alias_const(const type_t* t) {
  while (t->kind == TYPE_ALIAS)
    t = assertnotnull(((aliastype_t*)t)->elem);
  return t;
}


inline static type_t* unwind_alias(type_t* t) {
  while (t->kind == TYPE_ALIAS)
    t = assertnotnull(((aliastype_t*)t)->elem);
  return t;
}


bool types_isconvertible(const type_t* dst, const type_t* src) {
  dst = unwind_alias_const(assertnotnull(dst));
  src = unwind_alias_const(assertnotnull(src));
  if (dst == src)
    return true;
  if (type_isprim(dst) && type_isprim(src))
    return true;
  return false;
}


bool _types_iscompat(const compiler_t* c, const type_t* dst, const type_t* src) {
  dst = unwind_alias_const(assertnotnull(dst));
  src = unwind_alias_const(assertnotnull(src));
  switch (dst->kind) {
    case TYPE_INT:
      dst = canonical_primtype(c, dst);
      src = canonical_primtype(c, src);
      FALLTHROUGH;
    case TYPE_I8:
    case TYPE_I16:
    case TYPE_I32:
    case TYPE_I64:
      return (dst == src) && (dst->isunsigned == src->isunsigned);
    case TYPE_PTR:
      // *T <= *T
      // &T <= *T
      return (
        (src->kind == TYPE_PTR || src->kind == TYPE_REF) &&
        types_iscompat(c, ((ptrtype_t*)dst)->elem, ((ptrtype_t*)src)->elem));
    case TYPE_REF: {
      // &T    <= &T
      // mut&T <= &T
      // mut&T <= mut&T
      // &T    x= mut&T
      // &T    <= *T
      // mut&T <= *T
      const reftype_t* d = (const reftype_t*)dst;
      if (src->kind == TYPE_PTR)
        return types_iscompat(c, d->elem, ((const ptrtype_t*)src)->elem);
      const reftype_t* s = (const reftype_t*)src;
      return (
        src->kind == TYPE_REF &&
        (s->ismut == d->ismut || s->ismut || !d->ismut) &&
        types_iscompat(c, d->elem, s->elem) );
    }
    case TYPE_OPTIONAL: {
      // ?T <= T
      // ?T <= ?T
      const opttype_t* d = (const opttype_t*)dst;
      if (src->kind == TYPE_OPTIONAL)
        src = ((const opttype_t*)src)->elem;
      return types_iscompat(c, d->elem, src);
    }
  }
  return dst == src;
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
    dlog("%s replace %p with existing %p", __FUNCTION__, *tp, *p);
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


// unbox_id returns node->ref if node is an ID
static node_t* unbox_id(void* node) {
  node_t* n = node;
  while (n->kind == EXPR_ID)
    n = assertnotnull(((idexpr_t*)n)->ref);
  return n;
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
    return expr_no_side_effects(((const member_t*)n)->recv);
  case EXPR_FIELD:
  case EXPR_PARAM:
  case EXPR_LET:
  case EXPR_VAR: {
    const local_t* local = (const local_t*)n;
    return (
      type_cons_no_side_effects(local->type) &&
      ( !local->init || expr_no_side_effects(local->init) )
    );
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
  if (t->kind == TYPE_UNKNOWN)
    t = type_void;
  trace("typectx: %s -> %s", fmtnode(a, 0, a->typectx), fmtnode(a, 1, t));
  if UNLIKELY(!ptrarray_push(&a->typectxstack, a->ma, a->typectx))
    out_of_mem(a);
  a->typectx = t;
}

static void typectx_pop(typecheck_t* a) {
  assert(a->typectxstack.len > 0);
  type_t* t = ptrarray_pop(&a->typectxstack);
  trace("typectx: %s <- %s", fmtnode(a, 1, t), fmtnode(a, 0, a->typectx));
  a->typectx = t;
}


static void enter_scope(typecheck_t* a) {
  if (!scope_push(&a->scope, a->ma))
    out_of_mem(a);
}


static void leave_scope(typecheck_t* a) {
  scope_pop(&a->scope);
}


static node_t* nullable lookup(typecheck_t* a, sym_t name) {
  node_t* n = scope_lookup(&a->scope, name, U32_MAX);
  if (!n) {
    // look in package scope and its parent universe scope
    void** vp = map_lookup(&a->p->pkgdefs, name, strlen(name));
    if (!vp)
      return NULL;
    n = *vp;
  }
  n->nrefs++;
  return n;
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


static void type(typecheck_t* a, type_t** tp);
static void stmt(typecheck_t* a, stmt_t* n);
static void exprp(typecheck_t* a, expr_t** np);
#define expr(a, n)  exprp(a, (expr_t**)&(n))


static void check_unused(typecheck_t* a, const void* expr_node) {
  assert(node_isexpr(expr_node));
  const expr_t* n = expr_node;
  if (nodekind_islocal(n->kind)) {
    local_t* var = (local_t*)n;
    if (var->name != sym__ && noerror(a))
      warning(a, var->nameloc, "unused %s %s", nodekind_fmt(n->kind), var->name);
  } else if UNLIKELY(n->nrefs == 0 && n->kind != EXPR_IF && n->kind != EXPR_ASSIGN) {
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
      for (i++; i < count; i++) {
        if (node_isexpr(n->children.v[i]))
          ((expr_t*)n->children.v[i])->nrefs = 0;
      }
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

    if (nodekind_isexpr(cn->kind))
      check_unused(a, cn);
  }

  // if the block is rvalue, treat last entry as implicitly-returned expression
  if (stmt_end < count) {
    assert(n->flags & NF_RVALUE);
    expr_t* lastexpr = n->children.v[stmt_end];
    assert(nodekind_isexpr(lastexpr->kind));
    lastexpr->flags |= NF_RVALUE;
    expr(a, lastexpr);
    lastexpr->nrefs = MAX(n->nrefs, lastexpr->nrefs);
    n->type = lastexpr->type;
  }
}


static void block(typecheck_t* a, block_t* n) {
  enter_scope(a);
  block_noscope(a, n);
  leave_scope(a);
}


/* TODO typecheck "this" parameter

static type_t* this_param_type(parser_t* p, type_t* recvt, bool ismut) {
  if (!ismut) {
    // pass certain types as value instead of pointer when access is read-only
    if (nodekind_isprimtype(recvt->kind)) // e.g. int, i32
      return recvt;
    if (recvt->kind == TYPE_STRUCT) {
      // small structs
      structtype_t* st = (structtype_t*)recvt;
      usize ptrsize = p->scanner.compiler->ptrsize;
      if (st->align <= ptrsize && st->size <= ptrsize*2)
        return recvt;
    }
  }
  // pointer type
  reftype_t* t = mkreftype(p, ismut);
  t->elem = recvt;
  bubble_flags(t, t->elem);
  return (type_t*)t;
}


static void this_param(
  parser_t* p, funtype_t* ft, type_t* nullable recvt, local_t* param, bool ismut)
{
  if UNLIKELY(!recvt) {
    param->type = type_void;
    param->nrefs = 1; // prevent "unused parameter" warning
    error(p, (node_t*)param, "unexpected \"this\" parameter for plain function");
    return;
  }
  param->isthis = true;
  param->type = this_param_type(p, recvt, ismut);
}*/



static void funtype(typecheck_t* a, funtype_t** np) {
  funtype_t* ft = *np;
  typectx_push(a, type_void);
  for (u32 i = 0; i < ft->params.len; i++) {
    local_t* param = ft->params.v[i];
    if (param->isthis)
      dlog("TODO typecheck \"this\" parameter");
    type(a, &param->type);
  }
  type(a, &ft->result);
  typectx_pop(a);
  // TODO: consider NOT interning function types with parameters that have initializers
  intern_usertype(a->compiler, (usertype_t**)np);
}


static void fun(typecheck_t* a, fun_t* n) {
  fun_t* outer_fun = a->fun;
  a->fun = n;

  if (n->name && !n->methodof)
    define(a, n->name, n);

  // check function type first
  if CHECK_ONCE(n->type)
    funtype(a, (funtype_t**)&n->type);

  funtype_t* ft = (funtype_t*)n->type;
  assert(ft->kind == TYPE_FUN);

  // parameters
  if (ft->params.len > 0) {
    enter_scope(a);
    for (u32 i = 0; i < ft->params.len; i++) {
      local_t* param = ft->params.v[i];
      if CHECK_ONCE(param) {
        if (param->isthis)
          dlog("TODO typecheck \"this\" parameter");
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

  // body
  if (n->body) {
    n->body->flags |= NF_EXITS;
    if (ft->result != type_void)
      n->body->flags |= NF_RVALUE;
    typectx_push(a, ft->result);
    block(a, n->body);
    typectx_pop(a);
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

  if (ft->params.len > 0)
    scope_pop(&a->scope);

  a->fun = outer_fun;
}


#if 0
expr_t* nullable typecheck_if_cond(parser_t* p, expr_t* cond) {
  if (cond->type->kind == TYPE_BOOL)
    return NULL;

  dlog("TODO move check_if_cond to typecheck");

  if (!type_isopt(cond->type)) {
    error(p, cond, "conditional is not a boolean");
    return NULL;
  }

  // apply negation, e.g. "if (!x)"
  bool neg = false;
  while (cond->kind == EXPR_PREFIXOP) {
    unaryop_t* op = (unaryop_t*)cond;
    if UNLIKELY(op->op != OP_NOT) {
      error(p, cond, "invalid operation %s on optional type", fmtnode(p, 0, cond));
      return cond;
    }
    neg = !neg;
    cond = assertnotnull(op->expr);
  }

  // effective_type is either T or void, depending on "!" prefix ops.
  // e.g. "var x ?T ... ; if (x) { ... /* x is definitely T here */ }"
  // e.g. "var x ?T ... ; if (!x) { ... /* x is definitely void here */ }"
  type_t* effective_type = neg ? type_void : ((opttype_t*)cond->type)->elem;

  // redefine with effective_type
  switch (cond->kind) {
    case EXPR_ID: {
      // e.g. "if x { ... }"
      idexpr_t* id = (idexpr_t*)cond;
      if (!node_isexpr(id->ref)) {
        error(p, (node_t*)cond, "conditional is not an expression");
        return NULL;
      }

      idexpr_t* id2 = CLONE_NODE(p, id);
      id2->type = effective_type;

      expr_t* ref2 = (expr_t*)clone_node(p, id->ref);
      ref2->type = effective_type;
      define_replace(p, id->name, (node_t*)ref2);

      return (expr_t*)id2;
    }
    case EXPR_LET:
    case EXPR_VAR: {
      // e.g. "if let x = expr { ... }"
      ((local_t*)cond)->type = effective_type;
      cond->flags |= NF_OPTIONAL;
      break;
    }
  }

  return NULL;
}


expr_t* nullable typecheck_if_cond(parser_t* p, expr_t* cond) {
}
#endif


static void ifexpr(typecheck_t* a, ifexpr_t* n) {
  nodeflag_t extrafl = n->flags & NF_RVALUE;

  // "cond"
  assert(n->cond->flags & NF_RVALUE);
  enter_scope(a);
  expr(a, n->cond);

  // "then"
  enter_scope(a);
  n->thenb->flags |= extrafl;
  block_noscope(a, n->thenb);

  // "else"
  if (n->elseb) {
    // When there's an "else" branch, we need to fork the scopes of the
    // "then" and "else" blocks and then merge them (merge ownership during unwind.)
    // Stash the "then" scope away for now; we'll unwind it later
    if UNLIKELY(!scope_stash(&a->scope, a->ma))
      out_of_mem(a);

    // visit "else" branch
    enter_scope(a);
    n->elseb->flags |= extrafl;
    block_noscope(a, n->elseb);
    leave_scope(a);

    // restore stashed "then" scope
    scope_unstash(&a->scope);
  }

  // leave "then" scope
  leave_scope(a);

  // leave "cond" scope
  leave_scope(a);

  // type check
  if (n->flags & NF_RVALUE) {
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
}


static void idexpr(typecheck_t* a, idexpr_t* n) {
  if (!n->ref) {
    n->ref = lookup(a, n->name);
    if UNLIKELY(!n->ref) {
      error(a, n, "unknown identifier \"%s\"", n->name);
      return;
    }
  }
  if (node_istype(n->ref)) {
    n->type = (type_t*)n->ref;
  } else {
    n->type = asexpr(n->ref)->type;
  }
}


static void local(typecheck_t* a, local_t* n) {
  assertf(n->nrefs == 0 || n->name != sym__, "'_' local that is somehow referenced");
  if (n->init) {
    typectx_push(a, n->type);
    expr(a, n->init);
    typectx_pop(a);
    if (n->type == type_unknown || n->type->kind == TYPE_UNRESOLVED) {
      n->type = n->init->type;
    } else {
      check_types_iscompat(a, n, n->type, n->init->type);
    }
  }
  if UNLIKELY(n->type == type_void)
    error(a, n, "cannot define %s of type void", nodekind_fmt(n->kind));
  define(a, n->name, n);
}


static void local_var(typecheck_t* a, local_t* n) {
  assert(nodekind_isvar(n->kind));
  local(a, n);
}


static void retexpr(typecheck_t* a, retexpr_t* n) {
  if (n->value) {
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


static void binop(typecheck_t* a, binop_t* n) {
  expr(a, n->left);

  typectx_push(a, n->left->type);
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


static void assign(typecheck_t* a, binop_t* n) {
  binop(a, n);
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


static void intlit(typecheck_t* a, intlit_t* n) {
  if (n->type != type_unknown)
    return;

  u64 isneg = 0; // TODO

  type_t* type = a->typectx;
  type_t* basetype = unwind_alias(type);

  u64 maxval;
  u64 uintval = n->intval;
  if (isneg)
    uintval &= ~0x1000000000000000; // clear negative bit

  bool u = basetype->isunsigned;

  switch (basetype->kind) {
  case TYPE_I8:  maxval = u ? 0xffllu               : 0x7fllu+isneg; break;
  case TYPE_I16: maxval = u ? 0xffffllu             : 0x7fffllu+isneg; break;
  case TYPE_I32: maxval = u ? 0xffffffffllu         : 0x7fffffffllu+isneg; break;
  case TYPE_I64: maxval = u ? 0xffffffffffffffffllu : 0x7fffffffffffffffllu+isneg; break;
  case TYPE_INT:
    switch (a->compiler->intsize) {
      case 8/8:  maxval = u ? 0xffllu               : 0x7fllu+isneg; break;
      case 16/8: maxval = u ? 0xffffllu             : 0x7fffllu+isneg; break;
      case 32/8: maxval = u ? 0xffffffffllu         : 0x7fffffffllu+isneg; break;
      case 64/8: maxval = u ? 0xffffffffffffffffllu : 0x7fffffffffffffffllu+isneg; break;
      default:   assertf(0, "%u", a->compiler->intsize);
    }
    break;
  default:
    // all other type contexts results in int, uint, i64 or u64 (depending on value)
    if (a->compiler->intsize == 8) {
      if (isneg) {
        type = type_int;
        maxval = 0x8000000000000000llu;
      } else if (n->intval > 0x8000000000000000llu) {
        type = type_u64;
        maxval = 0xffffffffffffffffllu;
      } else {
        type = type_int;
        maxval = 0x7fffffffffffffffllu;
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
    error(a, n, "integer constant %s%llu overflows %s", isneg ? "-" : "", uintval, ts);
  }

  n->type = type;
}


static type_t* basetype(type_t* t) {
  // unwrap optional and ref
  assertnotnull(t);
  if (t->kind == TYPE_OPTIONAL)
    t = assertnotnull(((opttype_t*)t)->elem);
  if (t->kind == TYPE_REF)
    t = assertnotnull(((reftype_t*)t)->elem);
  return t;
}


static void member(typecheck_t* a, member_t* n) {
  expr(a, n->recv);

  if UNLIKELY(a->compiler->errcount)
    return;

  // get receiver type without ref or optional
  type_t* t = basetype(n->recv->type);

  expr_t* target = lookup_member(a->p, t, n->name);
  if UNLIKELY(!target) {
    n->type = a->typectx; // avoid cascading errors
    error(a, n, "%s has no field or method \"%s\"", fmtnode(a, 0, t), n->name);
    return;
  }

  n->target = target;
  n->type = n->target->type;
}


static void finalize_typecons(typecheck_t* a, typecons_t** np) {
  type_t* t = (*np)->type;

  if (!type_isprim(unwind_alias(t)))
    return;

  expr_t* expr = (*np)->expr;
  if (!expr)
    return;

  if (types_iscompat(a->compiler, t, expr->type)) {
    // eliminate type cast to equivalent type, e.g. "i8(3)" => "3"
    expr->nrefs += MAX(1, (*np)->nrefs) - 1;
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
  if (type_isprim(unwind_alias(t))) {
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
  type_t* basetype = unwind_alias(t);
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
  t = unwind_alias(t);

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

  node_t* recv = unbox_id(n->recv);
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
  sym_t name = (*tp)->name;
  type_t* t = (type_t*)lookup(a, name);

  // dlog("resolve type \"%s\" => %s %s",
  //   name, nodekind_name(t ? t->kind : 0), t ? fmtnode(a, 0, t) : "(null)");

  if LIKELY(t && nodekind_istype(t->kind)) {
    type(a, &t);
    t->nrefs += (*tp)->nrefs;
    *(type_t**)tp = t;
    return;
  }

  // not found
  if (!t) {
    error(a, *tp, "unknown type \"%s\"", name);
    return;
  }

  // not a type
  error(a, *tp, "%s %s is not a type", nodekind_fmt(t->kind), name);
  // redefine as "void" in current scope to minimize repetitive errors
  if (!scope_define(&a->scope, a->ma, name, *tp))
    out_of_mem(a);
}


static void typedef_(typecheck_t* a, typedef_t* n) {
  aliastype_t* at = &n->type;
  type(a, (type_t**)&at);
}


static void aliastype(typecheck_t* a, aliastype_t** tp) {
  aliastype_t* t = *tp;
  type(a, &t->elem);
  if UNLIKELY(t->elem == type_void)
    return error(a, t, "cannot alias type void");
}


// end call
// —————————————————————————————————————————————————————————————————————————————————


static void type1(typecheck_t* a, type_t** tp) {
  TRACE_NODE(a, "", tp);
  switch ((*tp)->kind) {
    case TYPE_UNRESOLVED: return unresolvedtype(a, (unresolvedtype_t**)tp);
    case TYPE_ALIAS:      return aliastype(a, (aliastype_t**)tp);
    case TYPE_FUN:        return funtype(a, (funtype_t**)tp);
  }
  dlog("TODO %s %s", __FUNCTION__, nodekind_name((*tp)->kind));
}


inline static void type(typecheck_t* a, type_t** tp) {
  if ((*tp)->flags & NF_CHECKED)
    return;
  (*tp)->flags |= NF_CHECKED;
  return type1(a, tp);
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
  case EXPR_FLOATLIT:
  case EXPR_BOOLLIT:
  case EXPR_FOR:
    panic("TODO %s", nodekind_name(n->kind));
    break;

  // We should never see these kinds of nodes
  case NODEKIND_COUNT:
  case NODE_BAD:
  case NODE_COMMENT:
  case NODE_UNIT:
  case STMT_TYPEDEF:
  case TYPE_VOID:
  case TYPE_BOOL:
  case TYPE_INT:
  case TYPE_I8:
  case TYPE_I16:
  case TYPE_I32:
  case TYPE_I64:
  case TYPE_F32:
  case TYPE_F64:
  case TYPE_ARRAY:
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


err_t typecheck(parser_t* p, unit_t* unit) {
  scope_clear(&p->scope);

  typecheck_t a = {
    .compiler = p->scanner.compiler,
    .p = p,
    .ma = p->scanner.compiler->ma,
    .ast_ma = p->ast_ma,
    .scope = p->scope,
    .typectx = type_void,
    .typectxstack = a.p->typectxstack,
  };

  ptrarray_clear(&a.typectxstack);

  enter_scope(&a);

  for (u32 i = 0; i < unit->children.len; i++)
    stmt(&a, unit->children.v[i]);

  leave_scope(&a);

  // update borrowed containers owned by p (in case they grew)
  p->scope = a.scope;
  p->typectxstack = a.p->typectxstack;

  return a.err;
}
