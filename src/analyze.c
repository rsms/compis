// static analysis pass
// SPDX-License-Identifier: Apache-2.0
#include "c0lib.h"
#include "compiler.h"

#define TRACE_ANALYSIS

#ifdef TRACE_ANALYSIS
  #define trace(fmt, va...) _dlog(4, "A", __FILE__, __LINE__, fmt, ##va)
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
} analysis_t;


typedef struct nref {
  node_t* n;
  struct nref* nullable parent;
} nref_t;


static const char* fmtnode(analysis_t* a, u32 bufindex, const node_t* n, u32 depth) {
  buf_t* buf = &a->p->tmpbuf[bufindex];
  buf_clear(buf);
  node_fmt(buf, n, depth);
  return buf->chars;
}


#ifdef TRACE_ANALYSIS
  static void trace_node(analysis_t* a, const char* msg, const node_t* n) {
    const char* str = fmtnode(a, 0, n, 0);
    trace("%s%-14s: %s", msg, nodekind_name(n->kind), str);
  }
#else
  #define trace_node(a,msg,n) ((void)0)
#endif


static void seterr(analysis_t* a, err_t err) {
  if (!a->err)
    a->err = err;
}


#define error(a, node_or_type, fmt, args...) \
  _error((a), (srcrange_t){ .focus = assertnotnull(node_or_type)->loc }, fmt, ##args)

ATTR_FORMAT(printf,3,4)
static void _error(analysis_t* a, srcrange_t srcrange, const char* fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  report_diagv(a->compiler, srcrange, DIAG_ERR, fmt, ap);
  va_end(ap);
  seterr(a, ErrInvalid);
}


static void out_of_mem(analysis_t* a) {
  seterr(a, ErrNoMem);
}


static void enter_scope(analysis_t* a) {
  if (!scope_push(&a->scope, a->ma))
    out_of_mem(a);
}


static void leave_scope(analysis_t* a, cleanuparray_t* cleanup, bool exits) {
  // TODO cleanup
  scope_pop(&a->scope);
}


static void leave_scope_TODO_cleanup(analysis_t* a, bool exits) {
  cleanuparray_t cleanup = {0};
  leave_scope(a, &cleanup, exits);
  if (cleanup.len) {
    cleanuparray_dispose(&cleanup, a->ast_ma);
    panic("TODO cleanup");
  }
}


static void define(analysis_t* a, sym_t name, void* n) {
  if (name == sym__)
    return;
  assert(scope_lookup(&a->scope, name, 0) == NULL); // parser checked this
  if (!scope_define(&a->scope, a->ma, name, n))
    out_of_mem(a);
}


#define DEF_SELF(self_node) \
  nref_t self = { .n = ((node_t*)(self_node)), .parent = &parent }


static void stmt(analysis_t* a, stmt_t* n, nref_t parent);
static void expr(analysis_t* a, expr_t* n, nref_t parent);


static void typedef_(analysis_t* a, typedef_t* n, nref_t parent) {
}


static void block_noscope(analysis_t* a, block_t* n, nref_t parent) {
  trace_node(a, "analyze ", (node_t*)n);

  if (n->children.len == 0)
    return;

  DEF_SELF(n);

  if ((n->flags & EX_RVALUE) == 0) {
    for (u32 i = 0; i < n->children.len; i++)
      stmt(a, n->children.v[i], self);
  } else {
    u32 last = n->children.len - 1;
    for (u32 i = 0; i < last; i++)
      stmt(a, n->children.v[i], self);
    expr_t* lastexpr = n->children.v[last];
    assert(nodekind_isexpr(lastexpr->kind));
    lastexpr->flags |= EX_RVALUE;
    expr(a, lastexpr, self);
  }
}


static void block(analysis_t* a, block_t* n, nref_t parent) {
  enter_scope(a);
  block_noscope(a, n, parent);
  leave_scope(a, &n->cleanup, n->flags & EX_EXITS);
}


static void fun(analysis_t* a, fun_t* n, nref_t parent) {
  DEF_SELF(n);

  if (n->name)
    define(a, n->name, n);

  if (n->params.len > 0) {
    for (u32 i = 0; i < n->params.len; i++)
      define(a, ((local_t*)n->params.v[i])->name, n->params.v[i]);
    enter_scope(a);
  }

  n->body->flags |= EX_EXITS;
  if (((funtype_t*)n->type)->result != type_void)
    n->body->flags |= EX_RVALUE;
  block(a, n->body, self);

  if (n->params.len > 0)
    leave_scope_TODO_cleanup(a, /*exits*/true);
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
    // stash the "then" scope away for now; we'll unwind it later
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
}


static void idexpr(analysis_t* a, idexpr_t* n, nref_t parent) {
  if ((n->flags & EX_RVALUE) == 0)
    return;

  if (type_isptr(n->type)) {
    dlog("TODO check use of owning %s \"%s\"", nodekind_name(n->kind), n->name);
  }
}


static void local(analysis_t* a, local_t* n, nref_t parent) {
  define(a, n->name, n);
  if (n->init) {
    DEF_SELF(n);
    expr(a, n->init, self);
  }
}


static void expr(analysis_t* a, expr_t* n, nref_t parent) {
  if (n->flags & EX_ANALYZED)
    return;
  n->flags |= EX_ANALYZED;

  trace_node(a, "analyze ", (node_t*)n);

  switch ((enum nodekind)n->kind) {
  case EXPR_FUN: return fun(a, (fun_t*)n, parent);
  case EXPR_IF:  return ifexpr(a, (ifexpr_t*)n, parent);
  case EXPR_ID:  return idexpr(a, (idexpr_t*)n, parent);

  case EXPR_FIELD:
  case EXPR_PARAM:
  case EXPR_VAR:
  case EXPR_LET:
    return local(a, (local_t*)n, parent);

  // TODO
  case EXPR_BLOCK:
  case EXPR_CALL:
  case EXPR_MEMBER:
  case EXPR_FOR:
  case EXPR_RETURN:
  case EXPR_DEREF:
  case EXPR_PREFIXOP:
  case EXPR_POSTFIXOP:
  case EXPR_BINOP:
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


static void stmt(analysis_t* a, stmt_t* n, nref_t parent) {
  trace_node(a, "analyze ", (node_t*)n);
  if (n->kind == STMT_TYPEDEF)
    return typedef_(a, (typedef_t*)n, parent);
  assertf(node_isexpr((node_t*)n), "unexpected node %s", nodekind_name(n->kind));
  return expr(a, (expr_t*)n, parent);
}


err_t analyze(parser_t* p, unit_t* unit) {
  scope_clear(&p->scope);
  analysis_t a = {
    .compiler = p->scanner.compiler,
    .p = p,
    .ma = p->scanner.compiler->ma,
    .ast_ma = p->ast_ma,
    .scope = p->scope,
  };

  nref_t self = { .n = (node_t*)unit };
  for (u32 i = 0; i < unit->children.len; i++)
    stmt(&a, unit->children.v[i], self);

  p->scope = a.scope; // in case it grew
  return a.err;
}
