// static analysis pass
// SPDX-License-Identifier: Apache-2.0
#include "c0lib.h"
#include "compiler.h"

#define TRACE_ANALYSIS

#ifdef TRACE_ANALYSIS
  #define trace(fmt, va...)  \
    _dlog(4, "A", __FILE__, __LINE__, "%*s" fmt, a->traceindent*2, "", ##va)
  #define tracex(fmt, va...) _dlog(4, "A", __FILE__, __LINE__, fmt, ##va)
#else
  #define trace(fmt, va...) ((void)0)
#endif


typedef struct {
  compiler_t* compiler;
  parser_t*   p;
  memalloc_t  ma;     // p->scanner.compiler->ma
  memalloc_t  ast_ma; // p->ast_ma
  scope_t     scope;
  err_t       err;
  type_t*     typectx;
  ptrarray_t  typectxstack;
  block_t*    block;
  ptrarray_t  blockstack;

  #ifdef TRACE_ANALYSIS
    int traceindent;
  #endif
} analysis_t;


typedef struct nref {
  node_t* n;
  struct nref* nullable parent;
} nref_t;


static const char* fmtnodex(
  analysis_t* a, u32 bufindex, const void* nullable n, u32 depth)
{
  buf_t* buf = &a->p->tmpbuf[bufindex];
  buf_clear(buf);
  node_fmt(buf, n, depth);
  return buf->chars;
}


static const char* fmtnode(analysis_t* a, u32 bufindex, const void* nullable n) {
  return fmtnodex(a, bufindex, n, 0);
}


#ifdef TRACE_ANALYSIS
  static void trace_node(analysis_t* a, const char* msg, const node_t* n) {
    const char* str = fmtnode(a, 0, n);
    trace("%s%-14s: %s", msg, nodekind_name(n->kind), str);
  }
  static void traceindent_decr(void* ap) {
    (*(analysis_t**)ap)->traceindent--;
  }
#else
  #define trace_node(a,msg,n) ((void)0)
#endif


static void seterr(analysis_t* a, err_t err) {
  if (!a->err)
    a->err = err;
}


ATTR_FORMAT(printf,3,4)
static void error(analysis_t* a, const void* nullable n, const char* fmt, ...) {
  srcrange_t srcrange = n ? node_srcrange(n) : (srcrange_t){0};
  va_list ap;
  va_start(ap, fmt);
  report_diagv(a->compiler, srcrange, DIAG_ERR, fmt, ap);
  va_end(ap);
}


ATTR_FORMAT(printf,3,4)
static void warning(analysis_t* a, const void* nullable n, const char* fmt, ...) {
  srcrange_t srcrange = n ? node_srcrange(n) : (srcrange_t){0};
  va_list ap;
  va_start(ap, fmt);
  report_diagv(a->compiler, srcrange, DIAG_WARN, fmt, ap);
  va_end(ap);
}


static void out_of_mem(analysis_t* a) {
  error(a, NULL, "out of memory");
  seterr(a, ErrNoMem);
}


static void* nullable lookup(analysis_t* a, sym_t name);


#define mknode(a, TYPE, kind)  ( (TYPE*)_mknode((a)->p, sizeof(TYPE), (kind)) )


// unbox_id returns node->ref if node is an ID
static node_t* unbox_id(void* node) {
  node_t* n = node;
  while (n->kind == EXPR_ID)
    n = ((idexpr_t*)n)->ref;
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


static local_t* nullable find_local(expr_t* n) {
  for (;;) switch (n->kind) {
    case EXPR_FIELD:
    case EXPR_PARAM:
    case EXPR_LET:
    case EXPR_VAR:
      return (local_t*)n;
    case EXPR_ID:
      if (((idexpr_t*)n)->ref && node_isexpr(((idexpr_t*)n)->ref)) {
        n = (expr_t*)((idexpr_t*)n)->ref;
        continue;
      }
      return NULL;
    default:
      return NULL;
  }
}


static void error_incompatible_types(
  analysis_t* a, const void* nullable origin_node, const type_t* x, const type_t* y)
{
  error(a, origin_node, "incompatible types, %s and %s",
    fmtnode(a, 0, x), fmtnode(a, 1, y));
}


static bool check_types_iscompat(
  analysis_t* a, const void* nullable origin_node,
  const type_t* nullable x, const type_t* nullable y)
{
  if UNLIKELY(!!x * !!y && !types_iscompat(x, y)) { // "!!x * !!y": ignore NULL
    error_incompatible_types(a, origin_node, x, y);
    return false;
  }
  return true;
}


static void typectx_push(analysis_t* a, type_t* t) {
  if UNLIKELY(!ptrarray_push(&a->typectxstack, a->ma, a->typectx))
    out_of_mem(a);
  a->typectx = t;
}

static void typectx_pop(analysis_t* a) {
  assert(a->typectxstack.len > 0);
  a->typectx = ptrarray_pop(&a->typectxstack);
}


static void blockctx_push(analysis_t* a, block_t* b) {
  if UNLIKELY(!ptrarray_push(&a->blockstack, a->ma, a->block))
    out_of_mem(a);
  a->block = b;
}

static void blockctx_pop(analysis_t* a) {
  assert(a->blockstack.len > 0);
  a->block = ptrarray_pop(&a->blockstack);
}


static void error_use_of_dead_ptr(analysis_t* a, void* origin_node, void* src_expr) {
  expr_t* src = (expr_t*)src_expr;
  if (nodekind_islocal(src->kind)) {
    local_t* local = (local_t*)src;
    error(a, origin_node,
      "attempt to use dead %s \"%s\"", nodekind_fmt(local->kind), local->name);
  } else {
    assert(nodekind_isexpr(src->kind));
    error(a, origin_node,
      "attempt to use dead %s %s", nodekind_fmt(src->kind), fmtnode(a, 0, src));
  }
}


static void ownership_transfer(
  analysis_t* a, scope_t* scope, expr_t* dstx, expr_t* src)
{
  // find destination
  expr_t* dst = (expr_t*)find_local(dstx);
  if (!dst)
    dst = dstx;

  assert(type_isowner(src->type));
  assert(type_isowner(dst->type));

  trace("ownership_transfer: %s %s -> %s %s",
    nodekind_fmt(src->kind), fmtnode(a, 0, src),
    nodekind_fmt(dst->kind), fmtnode(a, 1, dst));

  // shadow
  local_t* src_local = find_local(src);
  if (src_local) {
    if UNLIKELY(!owner_islive(src_local))
      return error_use_of_dead_ptr(a, src, src_local);

    local_t* src_local2 = CLONE_NODE(a->p, src_local);
    owner_setlive(src_local2, false);
    src_local2->flags |= EX_SHADOWS_OWNER;

    // define_replace(a, src_local2->name, (node_t*)src_local2);
    memalloc_t ma = scope == &a->scope ? a->ma : a->ast_ma;
    if UNLIKELY(!scope_define(scope, ma, src_local2->name, src_local2))
      out_of_mem(a);

    src_local->flags |= EX_OWNER_MOVED;
  } else {
    trace("  src is not a local, but %s", nodekind_name(src->kind));
    src->flags |= EX_OWNER_MOVED;
  }

  // mark destination as alive
  owner_setlive(dst, true);
}


static void transfer_value(
  analysis_t* a, void* origin, void* nullable dstp, void* nullable srcp)
{
  expr_t* dst = dstp; assert(dst == NULL || nodekind_isexpr(dst->kind));
  expr_t* src = srcp; assert(src == NULL || nodekind_isexpr(src->kind));

  return; // XXX IR

  if (a->compiler->errcount)
    return; // avoid reporting cascading errors

  if (dst == NULL) {
    if (!src)
      return;
    if (type_isowner(src->type)) {
      trace("kill %s", fmtnode(a, 1, src->type));
      owner_setlive(src, false);
    }
  } else if (type_isowner(dst->type)) {
    if (src) {
      trace("move %s -> %s", fmtnode(a, 0, src->type), fmtnode(a, 1, dst->type));
      if UNLIKELY(!type_isowner(src->type))
        return error_incompatible_types(a, origin, dst->type, src->type);
      ownership_transfer(a, &a->scope, dst, src);
    } else {
      trace("kill %s", fmtnode(a, 1, dst->type));
      owner_setlive(dst, false);
    }
  } else if (type_isref(dst->type)) {
    if UNLIKELY(src == NULL)
      return error(a, origin, "passing null to non-optional reference");
    trace("borrow %s -> %s", fmtnode(a, 0, src->type), fmtnode(a, 1, dst->type));
  } else if (src) {
    trace("copy %s -> %s", fmtnode(a, 0, src->type), fmtnode(a, 1, dst->type));
  } else {
    trace("zero %s", fmtnode(a, 1, dst->type));
  }
}


static void enter_scope(analysis_t* a) {
  if (!scope_push(&a->scope, a->ma))
    out_of_mem(a);
}


static void add_cleanup(analysis_t* a, ptrarray_t* cleanup, expr_t* owner) {
  assert(nodekind_islocal(owner->kind) || owner->kind == EXPR_CALL);
  if LIKELY(owner_islive(owner)) {
    trace("cleanup %s %s", nodekind_fmt(owner->kind), fmtnode(a, 0, owner->type));
    owner->nrefs++;
    if UNLIKELY(!ptrarray_push(cleanup, a->ast_ma, owner))
      out_of_mem(a);
  }
}


// returns true if the owner should be ignored
static bool abandon_owner(
  analysis_t* a, scope_t* scope, ptrarray_t* cleanup, bool exits,
  sym_t name, local_t* owner
) {
  assert(type_isowner(owner->type));

  // ignore type-narrowed shadow and moved-to-owner (e.g. return)
  if (owner->flags & (EX_SHADOWS_OPTIONAL | EX_OWNER_MOVED))
    return true;

  trace("%s: %s \"%s\" (%s)",
    __FUNCTION__, nodekind_fmt(owner->kind), name,
    owner_islive(owner) ? "live" : "dead");

  if ((owner->flags & EX_SHADOWS_OWNER) && !exits) {
    // the owner shadows a var in the outer scope
    trace("  shadow found");
    local_t* prev = scope_lookup(&a->scope, name, 0);
    if (prev) {
      // what is being shadowed is defined in this (the outer) scope
      trace("    mark %s \"%s\" DEAD", nodekind_fmt(prev->kind), name);
      assertnotnull(prev);
      assert(prev->kind == owner->kind);
      owner_setlive(prev, false);
    } else {
      // what owner shadows is defined further out; carry over the shadowing
      // owner into this (the outer) scope
      trace("    propagate %s \"%s\" to outer scope", nodekind_fmt(owner->kind), name);
      if UNLIKELY(!scope_define(&a->scope, a->ma, name, owner))
        out_of_mem(a);
    }
    return false;
  }

  add_cleanup(a, cleanup, (expr_t*)owner);
  return false;
}


static void unwind_scope(
  analysis_t* a, scope_t* scope, ptrarray_t* cleanup, bool exits)
{
  return; // XXX IR

  // TODO: iterate in reverse order
  for (u32 i = scope->base + 1; i < scope->len; i++) {
    node_t* n = assertnotnull(scope->ptr[i++]);
    sym_t name = assertnotnull(scope->ptr[i]);

    if (name == sym__ || !node_isexpr(n))
      continue;

    switch (n->kind) {
    case EXPR_FUN:
    case EXPR_ID:
      continue;
    case EXPR_LET:
    case EXPR_VAR:
    case EXPR_PARAM:
      if (((local_t*)n)->isthis)
        continue;
      if (type_isowner(((local_t*)n)->type)) {
        if (abandon_owner(a, scope, cleanup, exits, name, (local_t*)n))
          continue;
      }
      break;
    }

    if (((const expr_t*)n)->nrefs == 0)
      warning(a, n, "unused %s \"%s\"", nodekind_fmt(n->kind), name);
  }
}


static void leave_scope(analysis_t* a, ptrarray_t* cleanup, bool exits) {
  scope_t s = a->scope; // copy len & base before scope_pop
  scope_pop(&a->scope);
  unwind_scope(a, &s, cleanup, exits);
}


static void leave_scope_TODO_cleanup(analysis_t* a, bool exits) {
  ptrarray_t cleanup = {0};
  leave_scope(a, &cleanup, exits);
  if (cleanup.len)
    panic("TODO cleanup");
}


static void define(analysis_t* a, sym_t name, void* n) {
  if (name == sym__)
    return;

  trace("define \"%s\" => %s", name, fmtnode(a, 0, n));

  #if DEBUG
    node_t* existing = scope_lookup(&a->scope, name, 0);
    if (existing) {
      error(a, n, "duplicate definition \"%s\"", name);
      if (existing->loc.line)
        warning(a, existing, "previously defined here");
      assertf(0, "duplicate definition \"%s\"", name);
    }
  #endif

  if (!scope_define(&a->scope, a->ma, name, n))
    out_of_mem(a);
}


static void* nullable lookup(analysis_t* a, sym_t name) {
  node_t* n = scope_lookup(&a->scope, name, U32_MAX);
  if (n)
    return n;
  // look in package scope and its parent universe scope
  void** vp = map_lookup(&a->p->pkgdefs, name, strlen(name));
  return vp ? *vp : NULL;
}


#define DEF_SELF(self_node) \
  nref_t self = { .n = ((node_t*)(self_node)), .parent = &parent }


static void stmt(analysis_t* a, stmt_t* n, nref_t parent);
static void expr(analysis_t* a, expr_t* n, nref_t parent);


static void typedef_(analysis_t* a, typedef_t* n, nref_t parent) {
  dlog("TODO %s", __FUNCTION__);
}


static fun_t* nullable parent_fun(nref_t parent) {
  while (parent.n->kind != EXPR_FUN) {
    if (!parent.parent)
      return NULL;
    parent = *parent.parent;
  }
  return (fun_t*)parent.n;
}


static void check_unused(analysis_t* a, const void* expr_node) {
  assert(node_isexpr(expr_node));
  const expr_t* expr = expr_node;
  if UNLIKELY(expr->nrefs == 0)
    warning(a, expr, "unused %s %s", nodekind_fmt(expr->kind), fmtnode(a, 0, expr));
}


static void block_noscope(analysis_t* a, block_t* n, nref_t parent) {
  trace_node(a, "analyze ", (node_t*)n);

  if (n->children.len == 0)
    return;

  DEF_SELF(n);

  u32 count = n->children.len;
  u32 stmt_end = count - (u32)(n->flags & EX_RVALUE);
  blockctx_push(a, n);

  for (u32 i = 0; i < stmt_end; i++) {
    stmt_t* cn = n->children.v[i];
    stmt(a, cn, self);

    if (cn->kind == EXPR_RETURN) {
      // mark remaining expressions as unused
      // note: parser reports diagnostics about unreachable code
      for (i++; i < count; i++) {
        if (node_isexpr(n->children.v[i]))
          ((expr_t*)n->children.v[i])->nrefs = 0;
      }
      stmt_end = count; // avoid rvalue branch later on
      n->type = ((expr_t*)cn)->type;

      fun_t* fn = parent_fun(parent);
      if UNLIKELY(!fn) {
        error(a, cn, "return outside of function");
      } else if (n->type != type_void) {
        funtype_t* ft = (funtype_t*)fn->type;
        if UNLIKELY(ft->result == type_void) {
          error(a, cn, "function %s%sshould not return a value",
            fn->name ? fn->name : "", fn->name ? " " : "");
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
    assert(n->flags & EX_RVALUE);
    expr_t* lastexpr = n->children.v[stmt_end];
    assert(nodekind_isexpr(lastexpr->kind));
    lastexpr->flags |= EX_RVALUE;
    expr(a, lastexpr, self);
    lastexpr->nrefs = MAX(n->nrefs, lastexpr->nrefs);
    n->type = lastexpr->type;
    transfer_value(a, n, n, lastexpr);
  }

  blockctx_pop(a);
}


static void block(analysis_t* a, block_t* n, nref_t parent) {
  enter_scope(a);
  block_noscope(a, n, parent);
  leave_scope(a, &n->cleanup, n->flags & EX_EXITS);
}


static void fun(analysis_t* a, fun_t* n, nref_t parent) {
  DEF_SELF(n);

  if (n->name && !n->methodof)
    define(a, n->name, n);

  // parameters
  if (n->params.len > 0) {
    enter_scope(a);
    for (u32 i = 0; i < n->params.len; i++) {
      local_t* param = n->params.v[i];
      expr(a, (expr_t*)param, self);
    }
    if (!n->body) {
      scope_pop(&a->scope);
      return;
    }
  } else if (!n->body) {
    return;
  }

  funtype_t* ft = (funtype_t*)n->type;
  assert(ft->kind == TYPE_FUN);

  // body
  n->body->flags |= EX_EXITS;
  if (ft->result != type_void)
    n->body->flags |= EX_RVALUE;
  typectx_push(a, ft->result);
  block(a, n->body, self);
  typectx_pop(a);
  n->body->flags &= ~EX_RVALUE;

  if (n->params.len > 0)
    leave_scope(a, &n->body->cleanup, /*exits*/true);

  // check type of return value
  if UNLIKELY(ft->result != type_void && !types_iscompat(ft->result, n->body->type)) {
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


static void ifexpr(analysis_t* a, ifexpr_t* n, nref_t parent) {
  DEF_SELF(n);

  exprflag_t extrafl = n->flags & EX_RVALUE;

  // "cond"
  assert(n->cond->flags & EX_RVALUE);
  enter_scope(a);
  expr(a, n->cond, self);

  // "then"
  enter_scope(a);
  n->thenb->flags |= extrafl;
  block_noscope(a, n->thenb, self);

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
    block_noscope(a, n->elseb, self);
    leave_scope(a, &n->elseb->cleanup, n->elseb->flags & EX_EXITS);

    // restore stashed "then" scope
    scope_unstash(&a->scope);
  }

  // leave "then" scope
  leave_scope(a, &n->thenb->cleanup, n->thenb->flags & EX_EXITS);

  // leave "cond" scope
  leave_scope_TODO_cleanup(a, /*exits*/false);

  // type check
  if (n->flags & EX_RVALUE) {
    if (n->elseb && n->elseb->type != type_void) {
      // "if ... else" => T
      n->type = n->thenb->type;
      if UNLIKELY(!types_iscompat(n->thenb->type, n->elseb->type)) {
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


static void idexpr(analysis_t* a, idexpr_t* n, nref_t parent) {
  // check if owner is alive
  if ((n->flags & EX_RVALUE) && type_isowner(n->type)) {
    // first we need to do a fresh lookup in case there's a shadowing definition
    n->ref = assertnotnull( lookup(a, n->name) );
    local_t* src = (local_t*)n->ref;
    assert(nodekind_islocal(src->kind));
    assert(type_isowner(src->type));
    if UNLIKELY(!owner_islive(src))
      error_use_of_dead_ptr(a, n, src);
  }
}


static void local(analysis_t* a, local_t* n, nref_t parent) {
  assertf(n->nrefs == 0 || n->name != sym__, "'_' local that is somehow referenced");
  define(a, n->name, n);
  if (!n->init)
    return;
  DEF_SELF(n);
  typectx_push(a, n->type);
  expr(a, n->init, self);
  typectx_pop(a);
  if (n->type == type_void) {
    n->type = n->init->type;
  } else {
    check_types_iscompat(a, n, n->type, n->init->type);
  }
}


static void local_var(analysis_t* a, local_t* n, nref_t parent) {
  assert(nodekind_isvar(n->kind));
  local(a, n, parent);
  transfer_value(a, n, n, n->init);
}


static void retexpr(analysis_t* a, retexpr_t* n, nref_t parent) {
  if (n->value) {
    DEF_SELF(n);
    expr(a, n->value, self);
  }
  transfer_value(a, n, n, n->value);
}


static bool check_assign_to_member(analysis_t* a, member_t* m) {
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


static bool check_assign_to_id(analysis_t* a, idexpr_t* id) {
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


static bool check_assign(analysis_t* a, expr_t* target) {
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


static void binop(analysis_t* a, binop_t* n, nref_t parent) {
  DEF_SELF(n);

  expr(a, n->left, self);

  typectx_push(a, n->left->type);
  expr(a, n->right, self);
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


static void assign(analysis_t* a, binop_t* n, nref_t parent) {
  binop(a, n, parent);
  if (check_assign(a, n->left))
    transfer_value(a, n, n->left, n->right);
}


static void unaryop(analysis_t* a, unaryop_t* n, nref_t parent) {
  DEF_SELF(n);
  expr(a, n->expr, self);
  n->type = n->expr->type;
  if (n->op == TPLUSPLUS || n->op == TMINUSMINUS) {
    // TODO: specialized check here since it's not actually assignment (ownership et al)
    check_assign(a, n->expr);
  }
}


static void deref(analysis_t* a, unaryop_t* n, nref_t parent) {
  DEF_SELF(n);
  expr(a, n->expr, self);

  ptrtype_t* t = (ptrtype_t*)n->expr->type;
  if UNLIKELY(t->kind != TYPE_REF && t->kind != TYPE_PTR) {
    error(a, n, "dereferencing non-reference value of type %s", fmtnode(a, 0, t));
  } else {
    n->type = t->elem;
  }
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


static void member(analysis_t* a, member_t* n, nref_t parent) {
  DEF_SELF(n);

  expr(a, n->recv, self);

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


// —————————————————————————————————————————————————————————————————————————————————
// call


static void error_field_type(analysis_t* a, const expr_t* arg, const local_t* f) {
  const char* got = fmtnode(a, 0, arg->type);
  const char* expect = fmtnode(a, 1, f->type);
  const void* origin = arg;
  if (arg->kind == EXPR_PARAM)
    origin = assertnotnull(((local_t*)arg)->init);
  error(a, origin, "passing value of type %s for field \"%s\" of type %s",
    got, f->name, expect);
}


static void check_call_type_struct(
  analysis_t* a, call_t* call, structtype_t* t, nref_t self)
{
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
    arg->flags |= EX_RVALUE;

    typectx_push(a, f->type);

    if (arg->kind == EXPR_PARAM) {
      local_t* namedarg = (local_t*)arg;
      assertnotnull(namedarg->init); // checked by parser
      expr(a, namedarg->init, self);
      namedarg->type = namedarg->init->type;
    } else {
      assert(arg->kind == EXPR_ID); // for future dumb me
      idexpr(a, (idexpr_t*)arg, self);
    }

    typectx_pop(a);

    if UNLIKELY(!types_iscompat(f->type, arg->type))
      error_field_type(a, arg, f);
  }

  a->p->tmpmap = fieldmap; // in case map grew
}


static void call_type_prim(analysis_t* a, call_t* call, type_t* dst, nref_t self) {
  expr_t* arg = call->args.v[0];

  if UNLIKELY(!nodekind_isexpr(arg->kind))
    return error(a, arg, "invalid value");

  if UNLIKELY(arg->kind == EXPR_PARAM) {
    return error(a, arg, "%s type constructor does not accept named arguments",
      fmtnode(a, 0, dst));
  }

  typectx_push(a, dst);
  expr(a, arg, self);
  typectx_pop(a);

  type_t* src = arg->type;

  if UNLIKELY(types_iscompat(dst, src)) {
    warning(a, call, "cast to same type %s", fmtnode(a, 0, dst));
  } else if UNLIKELY(dst != src && !types_isconvertible(dst, src)) {
    const char* dst_s = fmtnode(a, 0, dst);
    const char* src_s = fmtnode(a, 1, src);
    error(a, arg, "cannot convert value of type %s to type %s", src_s, dst_s);
  }
}


static void error_call_type_arity(
  analysis_t* a, call_t* call, type_t* t, u32 minargs, u32 maxargs)
{
  assert(minargs > call->args.len || call->args.len > maxargs);
  const char* typstr = fmtnode(a, 1, t);
  if (call->args.len < minargs) {
    const void* origin = call->recv;
    if (call->args.len > 0)
      origin = call->args.v[call->args.len - 1];
    error(a, origin, "not enough arguments for %s type constructor, expecting%s %u",
      typstr, minargs != maxargs ? " at least" : "", minargs);
    return;
  }
  const node_t* arg = call->args.v[maxargs];
  const char* argstr = fmtnode(a, 0, arg);
  if (maxargs == 0) {
    // e.g. "void(x)"
    error(a, arg, "unexpected value %s; %s type accepts no arguments", argstr, typstr);
  } else {
    error(a, arg, "unexpected extra value %s in %s type constructor", argstr, typstr);
  }
}


static bool check_call_type_arity(
  analysis_t* a, call_t* call, type_t* t, u32 minargs, u32 maxargs)
{
  if UNLIKELY(minargs > call->args.len || call->args.len > maxargs) {
    error_call_type_arity(a, call, t, minargs, maxargs);
    return false;
  }
  return true;
}


static void call_type(analysis_t* a, call_t* call, type_t* t, nref_t self) {
  call->type = t;
  switch (t->kind) {
  case TYPE_VOID:
    // no arguments
    check_call_type_arity(a, call, t, 0, 0);
    return;

  case TYPE_BOOL:
  case TYPE_INT:
  case TYPE_I8:
  case TYPE_I16:
  case TYPE_I32:
  case TYPE_I64:
  case TYPE_F32:
  case TYPE_F64:
    if UNLIKELY(!check_call_type_arity(a, call, t, 1, 1))
      return;
    return call_type_prim(a, call, t, self);

  case TYPE_STRUCT: {
    u32 maxargs = ((structtype_t*)t)->fields.len;
    if UNLIKELY(!check_call_type_arity(a, call, t, 0, maxargs))
      return;
    return check_call_type_struct(a, call, (structtype_t*)t, self);
  }

  // TODO
  case TYPE_ARRAY:
    if UNLIKELY(!check_call_type_arity(a, call, t, 1, U32_MAX))
      return;
    FALLTHROUGH;
  case TYPE_ENUM:
  case TYPE_REF:
    trace("TODO IMPLEMENT %s", nodekind_name(t->kind));
    error(a, call->recv, "NOT IMPLEMENTED: %s", nodekind_name(t->kind));
    return;

  default:
    assertf(0,"unexpected %s", nodekind_name(t->kind));
    return;
  }
}


static void call_fun(analysis_t* a, call_t* call, funtype_t* ft, nref_t self) {
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
      expr(a, namedarg->init, self);
      arg->type = namedarg->init->type;
      seen_named_arg = true;

      if LIKELY(namedarg->name == param->name) {
        transfer_value(a, namedarg, param, namedarg->init);
      } else {
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
      expr(a, arg, self);
      transfer_value(a, arg, param, arg);
    }

    typectx_pop(a);

    // check type
    if UNLIKELY(!types_iscompat(param->type, arg->type)) {
      const char* got = fmtnode(a, 0, arg->type);
      const char* expect = fmtnode(a, 1, param->type);
      error(a, arg, "passing value of type %s to parameter of type %s", got, expect);
    }
  }

  if ((call->flags & EX_RVALUE) == 0 && type_isowner(call->type)) {
    // return value is owner, but it is not used (call is not rvalue)
    warning(a, call, "unused result; ownership transferred from function call");
    add_cleanup(a, &a->block->cleanup, (expr_t*)call);
  }
}


static void call(analysis_t* a, call_t* n, nref_t parent) {
  DEF_SELF(n);

  expr(a, n->recv, self);

  node_t* recv = unbox_id(n->recv);
  type_t* recvtype;

  if LIKELY(node_isexpr(recv)) {
    recvtype = ((expr_t*)recv)->type;
    if (recvtype->kind == TYPE_FUN)
      return call_fun(a, n, (funtype_t*)recvtype, self);
  } else if LIKELY(node_istype(recv)) {
    recvtype = (type_t*)recv;
    return call_type(a, n, recvtype, self);
  }

  // error: bad recv
  n->type = a->typectx; // avoid cascading errors
  if (node_isexpr(recv)) {
    error(a, n->recv,
      "calling an expression of type %s, expected function or type",
      fmtnode(a, 0, ((expr_t*)recv)->type));
  } else {
    error(a, n->recv,
      "calling %s; expected function or type", fmtnode(a, 0, recv));
  }
}


// end call
// —————————————————————————————————————————————————————————————————————————————————


static void stmt(analysis_t* a, stmt_t* n, nref_t parent) {
  if (n->kind == STMT_TYPEDEF) {
    trace_node(a, "analyze ", (node_t*)n);
    return typedef_(a, (typedef_t*)n, parent);
  }
  assertf(node_isexpr((node_t*)n), "unexpected node %s", nodekind_name(n->kind));
  return expr(a, (expr_t*)n, parent);
}


static void expr(analysis_t* a, expr_t* n, nref_t parent) {
  if (n->flags & EX_ANALYZED)
    return;
  n->flags |= EX_ANALYZED;

  #ifdef TRACE_ANALYSIS
    trace_node(a, "analyze ", (node_t*)n);
    a->traceindent++;
    analysis_t* a2 __attribute__((__cleanup__(traceindent_decr),__unused__)) = a;
  #endif

  switch ((enum nodekind)n->kind) {
  case EXPR_FUN:       return fun(a, (fun_t*)n, parent);
  case EXPR_IF:        return ifexpr(a, (ifexpr_t*)n, parent);
  case EXPR_ID:        return idexpr(a, (idexpr_t*)n, parent);
  case EXPR_RETURN:    return retexpr(a, (retexpr_t*)n, parent);
  case EXPR_BINOP:     return binop(a, (binop_t*)n, parent);
  case EXPR_ASSIGN:    return assign(a, (binop_t*)n, parent);
  case EXPR_BLOCK:     return block(a, (block_t*)n, parent);
  case EXPR_CALL:      return call(a, (call_t*)n, parent);
  case EXPR_MEMBER:    return member(a, (member_t*)n, parent);
  case EXPR_DEREF:     return deref(a, (unaryop_t*)n, parent);

  case EXPR_PREFIXOP:
  case EXPR_POSTFIXOP:
    return unaryop(a, (unaryop_t*)n, parent);

  case EXPR_FIELD:
  case EXPR_PARAM:
    return local(a, (local_t*)n, parent);
  case EXPR_VAR:
  case EXPR_LET:
    return local_var(a, (local_t*)n, parent);

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
  // Note: literals & constants always have flags&EX_ANALYZED set
  case EXPR_INTLIT:
  case EXPR_FLOATLIT:
  case EXPR_BOOLLIT:
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
  case TYPE_ENUM:
  case TYPE_FUN:
  case TYPE_PTR:
  case TYPE_REF:
  case TYPE_OPTIONAL:
  case TYPE_STRUCT:
    break;
  }
  assertf(0, "unexpected node %s", nodekind_name(n->kind));
}


err_t analyze(parser_t* p, unit_t* unit) {
  scope_clear(&p->scope);
  block_t unit_block = { .kind = EXPR_BLOCK, .type = type_void };

  analysis_t a = {
    .compiler = p->scanner.compiler,
    .p = p,
    .ma = p->scanner.compiler->ma,
    .ast_ma = p->ast_ma,
    .scope = p->scope,
    .typectx = type_void,
    .typectxstack = a.p->typectxstack,
    .block = &unit_block,
  };

  ptrarray_clear(&a.typectxstack);

  enter_scope(&a);

  nref_t self = { .n = (node_t*)unit };
  for (u32 i = 0; i < unit->children.len; i++)
    stmt(&a, unit->children.v[i], self);

  ptrarray_t cleanup = {0};
  leave_scope(&a, &cleanup, /*exits*/true);
  if UNLIKELY(cleanup.len) {
    dlog("unexpected top-level cleanup");
    seterr(&a, ErrInvalid);
  }

  assert(a.block == &unit_block);
  assert(unit_block.cleanup.cap == 0);

  ptrarray_dispose(&a.blockstack, a.ma);

  // update borrowed containers owned by p (in case they grew)
  p->scope = a.scope;
  p->typectxstack = a.p->typectxstack;

  return a.err;
}
