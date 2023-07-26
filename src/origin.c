// origin_t functions
// SPDX-License-Identifier: Apache-2.0
#include "colib.h"
#include "compiler.h"


origin_t funtype_params_origin(locmap_t* lm, const funtype_t* ft) {
  origin_t origin = origin_make(lm, ft->xxx_paramsloc);
  if (loc_line(ft->xxx_paramsloc) == loc_line(ft->xxx_paramsendloc))
    origin.width = loc_col(ft->xxx_paramsendloc) - origin.column + 1; // +1 for ")"
  return origin;
}


origin_t fun_params_origin(locmap_t* lm, const fun_t* fn) {
  origin_t origin = origin_make(lm, fn->paramsloc);
  if (loc_line(fn->paramsloc) == loc_line(fn->paramsendloc))
    origin.width = loc_col(fn->paramsendloc) - origin.column + 1; // +1 for ")"
  return origin;
}


origin_t ast_origin(locmap_t* lm, const node_t* n) {
  origin_t r = origin_make(lm, n->loc);
  switch (n->kind) {

  case STMT_TYPEDEF: {
    typedef_t* td = (typedef_t*)n;
    if (loc_line(td->type->loc))
      return origin_make(lm, td->type->loc);
    return r;
  }

  case EXPR_INTLIT:
    if (r.width == 0)
      r.width = (u32)ndigits10(((intlit_t*)n)->intval); // FIXME e.g. 0xbeef
    break;

  case EXPR_ID:
    r.width = strlen(((idexpr_t*)n)->name);
    break;

  case EXPR_DEREF:
    return origin_union(r, ast_origin(lm, (node_t*)((unaryop_t*)n)->expr));

  case EXPR_LET:
    return origin_make(lm, loc_union(((local_t*)n)->loc, ((local_t*)n)->nameloc));

  case EXPR_SUBSCRIPT:
    return origin_union(r, origin_make(lm, ((subscript_t*)n)->endloc));

  case EXPR_FUN: {
    fun_t* fn = (fun_t*)n;
    if (loc_line(fn->nameloc))
      r = origin_union(r, origin_make(lm, fn->nameloc));
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
    // note: r includes "("
    if (call->recv)
      r = origin_union(r, ast_origin(lm, (node_t*)call->recv));
    if (call->args.len > 0)
      r = origin_union(r, ast_origin(lm, call->args.v[call->args.len-1]));
    r = origin_union(r, origin_make(lm, call->argsendloc));
    break;
  }

  case TYPE_ARRAY: {
    const arraytype_t* t = (arraytype_t*)n;
    r = origin_union(r, ast_origin(lm, (node_t*)t->elem));
    if (t->lenexpr)
      r = origin_union(r, ast_origin(lm, (node_t*)t->lenexpr));
    r = origin_union(r, origin_make(lm, t->endloc));
    break;
  }

  case TYPE_SLICE: {
    const slicetype_t* t = (slicetype_t*)n;
    r = origin_union(r, ast_origin(lm, (node_t*)t->elem));
    r = origin_union(r, origin_make(lm, t->endloc));
    break;
  }

  case TYPE_UNRESOLVED:
    r.width = strlen(((unresolvedtype_t*)n)->name);
    break;

  }
  return r;
}