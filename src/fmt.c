#include "c0lib.h"
#include "compiler.h"
#include "abuf.h"


const char* nodekind_fmt(nodekind_t kind) {
  switch (kind) {
    case EXPR_PARAM:
      return "parameter";
    case EXPR_LET:
      return "binding";
    case EXPR_VAR:
      return "variable";
    case EXPR_FUN:
      return "function";
    case EXPR_BLOCK:
      return "block";
    case EXPR_ID:
      return "identifier";
    case EXPR_PREFIXOP:
    case EXPR_POSTFIXOP:
    case EXPR_BINOP:
      return "operation";
    case EXPR_INTLIT:
      return "literal constant";
    default:
      if (nodekind_istype(kind))
        return "type";
      if (nodekind_isexpr(kind))
        return "expression";
      return nodekind_name(kind);
  }
}


// static void fmtarray(abuf_t* s, const ptrarray_t* a, u32 depth, u32 maxdepth) {
//   for (usize i = 0; i < a->len; i++) {
//     abuf_c(s, ' ');
//     repr(s, a->v[i], indent, fl);
//   }
// }


static void startline(abuf_t* s, u32 indent) {
  if (s->len) abuf_c(s, '\n');
  abuf_fill(s, ' ', (usize)indent * 2);
}


static void fmt(abuf_t* s, const node_t* nullable n, u32 indent, u32 maxdepth) {
  if (maxdepth == 0)
    return;
  if (!n)
    return abuf_str(s, "(NULL)");
  switch ((enum nodekind)n->kind) {

  case NODE_UNIT: {
    const ptrarray_t* a = &((unit_t*)n)->children;
    for (u32 i = 0; i < a->len; i++) {
      startline(s, indent);
      fmt(s, a->v[i], indent, maxdepth - 1);
    }
    break;
  }

  case EXPR_VAR:
  case EXPR_LET:
    abuf_str(s, n->kind == EXPR_VAR ? "var " : "let ");
    FALLTHROUGH;
  case EXPR_PARAM:
    abuf_str(s, ((local_t*)n)->name);
    abuf_c(s, ' ');
    fmt(s, (node_t*)((local_t*)n)->type, indent, maxdepth);
    if (((local_t*)n)->init) {
      abuf_str(s, " = ");
      fmt(s, (node_t*)((local_t*)n)->init, indent, maxdepth);
    }
    break;

  case EXPR_FUN: {
    fun_t* fn = (fun_t*)n;
    abuf_fmt(s, "fun %s(", fn->name);
    for (u32 i = 0; i < fn->params.len; i++) {
      if (i > 0) abuf_str(s, ", ");
      fmt(s, fn->params.v[i], indent, maxdepth);
    }
    abuf_str(s, ") ");
    fmt(s, (node_t*)((funtype_t*)fn->type)->result, indent, maxdepth);
    if (fn->body) {
      abuf_c(s, ' ');
      fmt(s, (node_t*)fn->body, indent, maxdepth);
    }
    break;
  }

  case EXPR_BLOCK: {
    abuf_c(s, '{');
    const ptrarray_t* a = &((block_t*)n)->children;
    if (a->len > 0) {
      if (maxdepth == 1) {
        abuf_str(s, "...");
      } else {
        indent++;
        for (u32 i = 0; i < a->len; i++) {
          startline(s, indent);
          fmt(s, a->v[i], indent, maxdepth - 1);
        }
        indent--;
      }
    }
    startline(s, indent);
    abuf_c(s, '}');
    break;
  }

  case EXPR_CALL:
    dlog("TODO %s %s", __FUNCTION__, nodekind_name(n->kind));
    break;

  case EXPR_ID:
    abuf_str(s, ((idexpr_t*)n)->name);
    break;

  case EXPR_PREFIXOP:
    abuf_str(s, tok_repr(((unaryop_t*)n)->op));
    fmt(s, (node_t*)((unaryop_t*)n)->expr, indent, maxdepth - 1);
    break;

  case EXPR_POSTFIXOP:
    fmt(s, (node_t*)((unaryop_t*)n)->expr, indent, maxdepth - 1);
    abuf_str(s, tok_repr(((unaryop_t*)n)->op));
    break;

  case EXPR_BINOP:
    fmt(s, (node_t*)((binop_t*)n)->left, indent, maxdepth - 1);
    abuf_c(s, ' ');
    abuf_str(s, tok_repr(((binop_t*)n)->op));
    abuf_c(s, ' ');
    fmt(s, (node_t*)((binop_t*)n)->right, indent, maxdepth - 1);
    break;

  case EXPR_INTLIT: {
    const intlit_t* lit = (const intlit_t*)n;
    u32 base = 10;
    if (lit->type && lit->type->isunsigned)
      base = 16;
    abuf_u64(s, lit->intval, base);
    break;
  }

  case EXPR_FLOATLIT: {
    const floatlit_t* lit = (const floatlit_t*)n;
    double f64val = lit->type == type_f64 ? lit->f64val : (double)lit->f32val;
    abuf_f64(s, f64val, -1);
    break;
  }

  case TYPE_VOID: abuf_str(s, "void"); break;
  case TYPE_BOOL: abuf_str(s, "bool"); break;
  case TYPE_INT:  abuf_str(s, ((type_t*)n)->isunsigned ? "uint" : "int"); break;
  case TYPE_I8:   abuf_str(s, ((type_t*)n)->isunsigned ? "u8" : "i8"); break;
  case TYPE_I16:  abuf_str(s, ((type_t*)n)->isunsigned ? "u16" : "i16"); break;
  case TYPE_I32:  abuf_str(s, ((type_t*)n)->isunsigned ? "u32" : "i32"); break;
  case TYPE_I64:  abuf_str(s, ((type_t*)n)->isunsigned ? "u64" : "i64"); break;
  case TYPE_F32:  abuf_str(s, "f32"); break;
  case TYPE_F64:  abuf_str(s, "f64"); break;
  case TYPE_ARRAY: {
    arraytype_t* a = (arraytype_t*)n;
    abuf_fmt(s, "[%zu]", a->size);
    fmt(s, (node_t*)a->elem, indent, maxdepth);
    break;
  }
  case TYPE_ENUM:
  case TYPE_FUN:
  case TYPE_PTR:
  case TYPE_STRUCT:
    dlog("TODO %s", nodekind_name(n->kind));
    FALLTHROUGH;

  case NODE_BAD:
  case NODE_COMMENT:
  case NODEKIND_COUNT:
    abuf_str(s, nodekind_name(n->kind));
  }
}


err_t node_fmt(buf_t* buf, const node_t* n, u32 maxdepth) {
  usize needavail = 64;
  maxdepth = MAX(maxdepth, 1);
  for (;;) {
    buf_reserve(buf, needavail);
    abuf_t s = abuf_make(buf->p, buf->cap);
    fmt(&s, n, 0, maxdepth);
    usize len = abuf_terminate(&s);
    if (len < needavail) {
      buf->len += len;
      break;
    }
    needavail = len + 1;
  }
  return 0;
}
