// compile-time evaluation
// SPDX-License-Identifier: Apache-2.0
//
// FIXME TODO: this is a bit of a mess.
// Replace with something better and less complex.
// It currently evaluates the AST.
//
#include "colib.h"
#include "compiler.h"


#define TRACE_COMPTIME
#define TRACE_COMPTIME_RESULT  // also trace result of each evaluation


typedef array_type(u64) u64array_t;
DEF_ARRAY_TYPE_API(u64, u64array)


typedef struct {
  compiler_t* c;
  memalloc_t  ma;
  memalloc_t  ast_ma;
  u64array_t  stack;
  err_t       err;

  // call frame state
  expr_t* nullable   returnval;
  map_t              localm; // {local_t* => node_t*}
  local_t** nullable localv;
  u32                localc;

  // constants lazily allocated in ast_ma
  intlit_t* nullable const_true;
  intlit_t* nullable const_false;

  #ifdef TRACE_COMPTIME
    int traceindent;
  #endif
} ctx_t;


#if defined(TRACE_COMPTIME) && defined(CO_DEVBUILD)
  #define trace(fmt, va...)  \
    _dlog(5, "eval", __FILE__, __LINE__, "%*s" fmt, ctx->traceindent*2, "", ##va)

  // static void _trace_scope_end(ctx_t** cp) { (*cp)->traceindent--; }
  // #define TRACE_SCOPE() \
  //   ctx->traceindent++; \
  //   ctx_t* __tracer __attribute__((__cleanup__(_trace_scope_end),__unused__)) = ctx
#else
  #undef TRACE_COMPTIME
  #undef TRACE_COMPTIME_RESULT
  #define trace(fmt, va...) ((void)0)
  // #define TRACE_SCOPE()     ((void)0)
#endif


static void seterr(ctx_t* ctx, err_t err) {
  if (!ctx->err)
    ctx->err = err;
}


// const origin_t to_origin(typecheck_t*, T origin)
// where T is one of: origin_t | loc_t | node_t* (default)
#define to_origin(ctx, origin) ({ \
  __typeof__(origin)* __tmp = &origin; \
  const origin_t __origin = _Generic(__tmp, \
          origin_t*:  *(origin_t*)__tmp, \
    const origin_t*:  *(origin_t*)__tmp, \
          loc_t*:     origin_make(&(ctx)->c->locmap, *(loc_t*)__tmp), \
    const loc_t*:     origin_make(&(ctx)->c->locmap, *(loc_t*)__tmp), \
          default:    node_origin(&(ctx)->c->locmap, *(node_t**)__tmp) \
  ); \
  __origin; \
})


// void diag(typecheck_t*, T origin, diagkind_t diagkind, const char* fmt, ...)
// where T is one of: origin_t | loc_t | node_t* | expr_t*
#define diag(ctx, origin, diagkind, fmt, args...) \
  report_diag((ctx)->c, to_origin((ctx), (origin)), (diagkind), fmt, ##args)

#define error(ctx, origin, fmt, args...)   diag(ctx, origin, DIAG_ERR, fmt, ##args)
#define warning(ctx, origin, fmt, args...) diag(ctx, origin, DIAG_WARN, fmt, ##args)
#define help(ctx, origin, fmt, args...)    diag(ctx, origin, DIAG_HELP, fmt, ##args)

// static void* error_not_supported(ctx_t* ctx, node_t* origin, const char* fmt, ...)
#define error_not_supported(ctx, origin, fmt, args...) \
  diag((ctx), (origin), DIAG_ERR, fmt " not supported at compile time", ##args)


static void* error_operation_not_supported(
  ctx_t* ctx, void* origin, op_t op, const char* typename)
{
  error_not_supported(ctx, origin, "operation %s on %s", op_name(op) + 3, typename);
  return origin;
}


UNUSED static const char* fmtnode(ctx_t* ctx, u32 bufindex, const void* nullable n) {
  buf_t* buf = tmpbuf(bufindex);
  err_t err = node_fmt(buf, n, /*depth*/0);
  if (!err)
    return buf->chars;
  dlog("node_fmt: %s", err_str(err));
  seterr(ctx, err);
  return "?";
}


#define mknode(ctx, TYPE, kind, loc) \
  ( (TYPE*)_mknode1((ctx), sizeof(TYPE), (kind), (loc)) )

static node_t* _mknode1(ctx_t* ctx, usize size, nodekind_t kind, loc_t loc) {
  mem_t m = mem_alloc_zeroed(ctx->ast_ma, size);
  if UNLIKELY(m.p == NULL)
    return seterr(ctx, ErrNoMem), last_resort_node;
  node_t* n = m.p;
  n->kind = kind;
  n->loc = loc;
  return n;
}


#define STACK_PUSH(ctx, value) \
  if UNLIKELY(!u64array_push(&(ctx)->stack, (ctx)->ma, (u64)(uintptr)(value))) \
    return seterr((ctx), ErrNoMem), last_resort_node

#define STACK_POP(ctx) \
  ( assert((ctx)->stack.len > 0), (ctx)->stack.len-- )


static void* eval(ctx_t* ctx, void* node);


static void* lookup_local(ctx_t* ctx, local_t* n) {
  void** vp = map_lookup_ptr(&ctx->localm, n);
  if (!vp)
    return error(ctx, n, "undefined local '%s'", n->name), n;
  return assertnotnull(*vp);
}


static void define_local(ctx_t* ctx, local_t* n, void* value) {
  void** vp = map_assign_ptr(&ctx->localm, ctx->ma, n);
  if (!vp)
    return seterr(ctx, ErrNoMem);
  *vp = assertnotnull(value);
}


static void* localdefinition(ctx_t* ctx, local_t* n) {
  expr_t* init = n->init;
  if (!init)
    return error_not_supported(ctx, n, "variable declaration without initializer"), n;
  define_local(ctx, n, init);
  return init;
}


static void* idexpr(ctx_t* ctx, idexpr_t* n) {
  node_t* ref = assertnotnull(n->ref);
  switch (ref->kind) {
    case EXPR_VAR:
    case EXPR_LET:
    case EXPR_PARAM:
      ref = lookup_local(ctx, (local_t*)ref);
      break;
  }
  return eval(ctx, ref);
}


static void* call(ctx_t* ctx, call_t* n) {
  fun_t* recv = eval(ctx, n->recv);
  assert_nodekind(recv, EXPR_FUN);
  // TODO check if receiver function has a closure
  if (recv->body == NULL)
    return error(ctx, n, "call to function without implementation"), n;

  // push return status
  // STACK_PUSH(ctx, 0);

  // save current function state
  expr_t* returnval = ctx->returnval;
  local_t** localv  = ctx->localv;
  u32 localc        = ctx->localc;

  // setup new function state
  funtype_t* ft = (funtype_t*)recv->type;
  assert( ft->params.len == n->args.len );
  for (u32 i = 0; i < n->args.len; i++) {
    expr_t* arg = n->args.v[i];
    if (arg->kind == EXPR_PARAM) // named argument
      arg = ((local_t*)arg)->init;
    local_t* param = ft->params.v[i];
    define_local(ctx, param, arg);
  }
  if (ctx->err) // define_local failed
    return n;
  ctx->returnval = NULL;

  // evaluate function
  eval(ctx, recv->body);

  // save result in a local temp
  expr_t* result = ctx->returnval;
  if (!result)
    result = (expr_t*)&last_resort_node;

  // restore current function state
  ctx->returnval = returnval;
  ctx->localv    = localv;
  ctx->localc    = localc;

  return result;
}


static void* block(ctx_t* ctx, block_t* n) {
  void* result = last_resort_node;
  for (u32 i = 0; i < n->children.len && ctx->returnval == NULL && ctx->err == 0; i++)
    result = eval(ctx, n->children.v[i]);
  return result;
}


static intlit_t* mkbool(ctx_t* ctx, u64 intval) {
  intlit_t* n = mknode(ctx, intlit_t, EXPR_BOOLLIT, 0);
  n->intval = intval;
  n->type = type_bool;
  return n;
}


inline static intlit_t* const_bool(ctx_t* ctx, bool value) {
  return value ? \
    LIKELY(ctx->const_true)  ? ctx->const_true  : (ctx->const_true  = mkbool(ctx, 1)) :
    LIKELY(ctx->const_false) ? ctx->const_false : (ctx->const_false = mkbool(ctx, 0));
}


static void* binop_test_shortcircuit(ctx_t* ctx, binop_t* n, expr_t* l) {
  // "&&" and "||" short-circuit
  if UNLIKELY(l->type != type_bool)
    return error(ctx, n->left, "expected boolean"), l;

  // "l && r"
  if (n->op == OP_LAND) {
    if (((intlit_t*)l)->intval) {
      intlit_t* r = eval(ctx, n->right);
      if UNLIKELY(r->kind != EXPR_INTLIT || r->type != type_bool)
        return error(ctx, n->right, "expected boolean"), l;
      return const_bool(ctx, r->intval);
    } else {
      return const_bool(ctx, 0);
    }
  }

  // "l || r"
  if (((intlit_t*)l)->intval) {
    return const_bool(ctx, 1);
  } else {
    intlit_t* r = eval(ctx, n->right);
    if UNLIKELY(r->kind != EXPR_INTLIT || r->type != type_bool)
      return error(ctx, n->right, "expected boolean"), l;
    return const_bool(ctx, r->intval);
  }
}


static void* binop_float(ctx_t* ctx, binop_t* n, floatlit_t* l, floatlit_t* r) {
  return error_operation_not_supported(ctx, n, n->op, nodekind_name(l->kind));
}

static void* binop_int(ctx_t* ctx, binop_t* n, intlit_t* l, intlit_t* r) {
  #define OPSWITCH \
    switch (n->op) { \
    case OP_ADD:  res = (lv +  rv); goto ok; \
    case OP_SUB:  res = (lv -  rv); goto ok; \
    case OP_MUL:  res = (lv *  rv); goto ok; \
    case OP_DIV:  res = (lv /  rv); goto ok; \
    case OP_MOD:  res = (lv %  rv); goto ok; \
    case OP_AND:  res = (lv &  rv); goto ok; \
    case OP_OR:   res = (lv |  rv); goto ok; \
    case OP_XOR:  res = (lv ^  rv); goto ok; \
    case OP_SHL:  res = (lv << rv); goto ok; \
    case OP_SHR:  res = (lv >> rv); goto ok; \
    case OP_EQ:   res = (lv == rv); goto ok; \
    case OP_NEQ:  res = (lv != rv); goto ok; \
    case OP_LT:   res = (lv <  rv); goto ok; \
    case OP_GT:   res = (lv >  rv); goto ok; \
    case OP_LTEQ: res = (lv <= rv); goto ok; \
    case OP_GTEQ: res = (lv >= rv); goto ok; \
    }

  u64 res = 0;
  nodekind_t kind = l->type->kind;
again:
  switch (kind) {
    case TYPE_INT:  kind = ctx->c->inttype->kind; goto again;
    case TYPE_UINT: kind = ctx->c->uinttype->kind; goto again;

    case TYPE_I8:  { i8  lv = (i8)l->intval,  rv = (i8)r->intval; OPSWITCH break; }
    case TYPE_I16: { i16 lv = (i16)l->intval, rv = (i16)r->intval; OPSWITCH break; }
    case TYPE_I32: { i32 lv = (i32)l->intval, rv = (i32)r->intval; OPSWITCH break; }
    case TYPE_I64: { i64 lv = (i64)l->intval, rv = (i64)r->intval; OPSWITCH break; }
    case TYPE_U8:  { u8  lv = (u8)l->intval,  rv = (u8)r->intval; OPSWITCH break; }
    case TYPE_U16: { u16 lv = (u16)l->intval, rv = (u16)r->intval; OPSWITCH break; }
    case TYPE_U32: { u32 lv = (u32)l->intval, rv = (u32)r->intval; OPSWITCH break; }
    case TYPE_U64: { u64 lv = (u64)l->intval, rv = (u64)r->intval; OPSWITCH break; }
  }

  #undef OPSWITCH

  return error_operation_not_supported(ctx, n, n->op, "integers");
ok:
  if (res == l->intval)
    return l;
  if (res == r->intval)
    return r;
  intlit_t* result = mknode(ctx, intlit_t, EXPR_INTLIT, 0);
  result->intval = res;
  result->type = l->type;
  return result;
}


static void* binop(ctx_t* ctx, binop_t* n) {
  expr_t* l = asexpr(eval(ctx, n->left));
  if (n->op == OP_LAND || n->op == OP_LOR)
    return binop_test_shortcircuit(ctx, n, l);

  expr_t* r = asexpr(eval(ctx, n->right));
  if (l->kind == EXPR_FLOATLIT) {
    assert(r->kind == EXPR_FLOATLIT);
    return binop_float(ctx, n, (floatlit_t*)l, (floatlit_t*)r);
  }

  if (l->kind == EXPR_INTLIT) {
    assert(r->kind == EXPR_INTLIT);
    return binop_int(ctx, n, (intlit_t*)l, (intlit_t*)r);
  }

  return error_operation_not_supported(ctx, n, n->op, nodekind_name(l->kind));
}


static void* assign(ctx_t* ctx, binop_t* n) {
  if (n->left->kind != EXPR_ID) {
    error_not_supported(ctx, n, "%s with %s",
      nodekind_fmt(n->kind), nodekind_fmt(n->left->kind));
    return n;
  }

  // assignment to local

  node_t* ref = assertnotnull(((idexpr_t*)n->left)->ref);
  switch (ref->kind) {
    case EXPR_LET:
    case EXPR_VAR:
    case EXPR_PARAM:
      define_local(ctx, (local_t*)ref, n->right);
      break;
    default:
      assertf(0, "unexpected %s", nodekind_name(ref->kind));
  }
  return n->right;
}


static void* retexpr(ctx_t* ctx, retexpr_t* n) {
  if (n->value) {
    ctx->returnval = eval(ctx, n->value);
  } else {
    // no return value, but we have to set ctx->returnval to something
    ctx->returnval = (expr_t*)&last_resort_node;
  }
  return ctx->returnval;
}


static void* eval1(ctx_t* ctx, void* np);

#ifdef TRACE_COMPTIME
  static void* eval(ctx_t* ctx, void* np) {
    node_t* n = np;

    ctx->traceindent++;
    buf_t* buf0 = tmpbuf(0);
    buf_clear(buf0);
    node_fmt(buf0, (node_t*)n, 0);
    trace("→ %s %s ...", nodekind_name(n->kind), buf0->chars);

    node_t* result = eval1(ctx, np);

    assertnotnull(result);

    #ifdef TRACE_COMPTIME_RESULT
      buf_t* buf1 = tmpbuf(1);
      buf_clear(buf0);
      buf_clear(buf1);
      node_fmt(buf0, (node_t*)n, 0);
      node_fmt(buf1, result, 0);

      trace("  %s %s => %s %s <%s>",
        nodekind_name(n->kind), buf0->chars,
        nodekind_name(result->kind), buf1->chars,
        node_isexpr(result) ? nodekind_name(asexpr(result)->type->kind) : "type");
    #endif

    ctx->traceindent--;

    return result;
  }
#else
  __attribute__((always_inline))
  inline static void* eval(ctx_t* ctx, void* np) { return eval1(ctx, np); }
#endif


static void* eval1(ctx_t* ctx, void* np) {
  node_t* n = np;
  switch ((enum nodekind)n->kind) {

  // terminals
  case EXPR_BOOLLIT:
  case EXPR_INTLIT:
  case EXPR_FLOATLIT:
  case EXPR_STRLIT:
    return n;

  case EXPR_ID:     return idexpr(ctx, np);
  case EXPR_CALL:   return call(ctx, np);
  case EXPR_BLOCK:  return block(ctx, np);
  case EXPR_BINOP:  return binop(ctx, np);
  case EXPR_RETURN: return retexpr(ctx, np);
  case EXPR_FUN:    return np;
  case EXPR_ASSIGN: return assign(ctx, np);

  case EXPR_VAR:
  case EXPR_LET:
    return localdefinition(ctx, np);

  // —— TODO ——
  case NODE_UNIT:
  case STMT_TYPEDEF:

  case EXPR_TYPECONS:
  case EXPR_MEMBER:
  case EXPR_IF:
  case EXPR_FOR:
  case EXPR_DEREF:
  case EXPR_PREFIXOP:
  case EXPR_POSTFIXOP:

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
  case TYPE_STRUCT:
  case TYPE_FUN:
  case TYPE_ARRAY:
  case TYPE_SLICE:
  case TYPE_MUTSLICE:
  case TYPE_PTR:
  case TYPE_REF:
  case TYPE_MUTREF:
  case TYPE_OPTIONAL:
  case TYPE_ALIAS:
  case TYPE_UNKNOWN:
  case TYPE_UNRESOLVED:
    return error_not_supported(ctx, n, "%s", nodekind_fmt(n->kind)), n;

  // nodes we should never encounter as an expression
  case NODE_BAD:
  case NODE_COMMENT:
  case NODEKIND_COUNT:
  case EXPR_PARAM:
  case EXPR_FIELD:
    break;
  }
  assertf(0, "unexpected node %s", nodekind_name(n->kind));
  return n;
}


node_t* nullable comptime_eval(compiler_t* c, expr_t* expr) {
  ctx_t ctx = {
    .c = c,
    .ma = c->ma,
    .ast_ma = c->ma, // TODO FIXME pass as function argument
  };

  if (!map_init(&ctx.localm, ctx.ma, 16))
    return NULL;

  u32 errcount = c->errcount;

  node_t* result = eval(&ctx, expr);

  if UNLIKELY(c->errcount > errcount && loc_line(expr->loc))
    help(&ctx, expr, "comptime evaluation originated here");

  u64array_dispose(&ctx.stack, ctx.ma);
  map_dispose(&ctx.localm, ctx.ma);

  return result;
}


bool comptime_eval_uint(compiler_t* c, expr_t* expr, u64* result) {
  intlit_t* n;
  if (expr->kind == EXPR_INTLIT) {
    // shortcut for common case, e.g. "3" in "var myarray [int 3]"
    n = (intlit_t*)expr;
  } else {
    n = (intlit_t*)comptime_eval(c, expr);
    if (!n)
      return false;
  }

  if LIKELY(n->kind == EXPR_INTLIT && n->type == type_uint) {
    *result = n->intval;
    return true;
  }

  // error
  origin_t origin = node_origin(&c->locmap, (node_t*)expr);
  report_diag(c, origin, DIAG_ERR, "expression does not result in a value of type uint");
  return false;
}
