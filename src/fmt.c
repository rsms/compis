// SPDX-License-Identifier: Apache-2.0
#include "c0lib.h"
#include "compiler.h"
#include "abuf.h"

// TODO: rewrite this to use buf_t, like ast.c and cgen.c


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
    case EXPR_DEREF:
      return "dereference";
    case EXPR_INTLIT:
      return "literal constant";
    case EXPR_MEMBER:
      return "struct field";
    case TYPE_STRUCT:
      return "struct type";
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


static void fmt(abuf_t* s, const node_t* nullable n, u32 indent, u32 maxdepth);


static void local(abuf_t* s, const local_t* nullable n, u32 indent, u32 maxdepth) {
  abuf_str(s, n->name);
  abuf_c(s, ' ');
  fmt(s, (node_t*)n->type, indent, maxdepth);
  if (n->init && maxdepth > 1) {
    abuf_str(s, " = ");
    fmt(s, (node_t*)n->init, indent, maxdepth);
  }
}


static void funtype(abuf_t* s, const funtype_t* nullable n, u32 indent, u32 maxdepth) {
  assert(maxdepth > 0);
  abuf_c(s, '(');
  for (u32 i = 0; i < n->params.len; i++) {
    if (i) abuf_str(s, ", ");
    const local_t* param = n->params.v[i];
    abuf_str(s, param->name);
    if (i+1 == n->params.len || ((local_t*)n->params.v[i+1])->type != param->type) {
      abuf_c(s, ' ');
      fmt(s, (const node_t*)param->type, indent, maxdepth);
    }
  }
  abuf_str(s, ") ");
  fmt(s, (const node_t*)n->result, indent, maxdepth);
}


static void structtype(
  abuf_t* s, const structtype_t* nullable t, u32 indent, u32 maxdepth)
{
  if (t->name)
    abuf_str(s, t->name);
  if (maxdepth <= 1) {
    if (!t->name)
      abuf_str(s, "struct");
    return;
  }
  if (t->name)
    abuf_c(s, ' ');
  abuf_c(s, '{');
  if (t->fields.len > 0) {
    indent++;
    for (u32 i = 0; i < t->fields.len; i++) {
      startline(s, indent);
      const local_t* f = t->fields.v[i];
      abuf_str(s, f->name), abuf_c(s, ' ');
      fmt(s, (const node_t*)f->type, indent, maxdepth);
      if (f->init) {
        abuf_str(s, " = ");
        fmt(s, (const node_t*)f->init, indent, maxdepth);
      }
    }
    indent--;
    startline(s, indent);
  }
  abuf_c(s, '}');
}


static void fmt_nodelist(
  abuf_t* s, const ptrarray_t* nodes, const char* sep, u32 indent, u32 maxdepth)
{
  for (u32 i = 0; i < nodes->len; i++) {
    if (i) abuf_str(s, sep);
    fmt(s, nodes->v[i], indent, maxdepth);
  }
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

  case STMT_TYPEDEF:
    abuf_fmt(s, "type %s", ((typedef_t*)n)->name);
    if (maxdepth > 1) {
      abuf_c(s, ' ');
      fmt(s, (node_t*)((typedef_t*)n)->type, indent, maxdepth - 1);
    }
    break;

  case EXPR_VAR:
  case EXPR_LET:
    abuf_str(s, n->kind == EXPR_VAR ? "var " : "let ");
    FALLTHROUGH;
  case EXPR_PARAM:
    return local(s, (const local_t*)n, indent, maxdepth);

  case EXPR_FUN: {
    fun_t* fn = (fun_t*)n;
    abuf_fmt(s, "fun %s(", fn->name);
    fmt_nodelist(s, &fn->params, ", ", indent, maxdepth);
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
        startline(s, indent);
      }
    }
    abuf_c(s, '}');
    break;
  }

  case EXPR_CALL: {
    const call_t* call = (const call_t*)n;
    fmt(s, (const node_t*)call->recv, indent, maxdepth);
    abuf_c(s, '(');
    fmt_nodelist(s, &call->args, ", ", indent, maxdepth);
    abuf_c(s, ')');
    break;
  }

  case EXPR_MEMBER:
    fmt(s, (node_t*)((const member_t*)n)->recv, indent, maxdepth);
    abuf_c(s, '.');
    abuf_str(s, ((const member_t*)n)->name);
    break;

  case EXPR_IF:
    abuf_str(s, "if ");
    fmt(s, (node_t*)((const ifexpr_t*)n)->cond, indent, maxdepth);
    abuf_c(s, ' ');
    fmt(s, (node_t*)((const ifexpr_t*)n)->thenb, indent, maxdepth);
    if (((const ifexpr_t*)n)->elseb) {
      abuf_str(s, " else ");
      fmt(s, (node_t*)((const ifexpr_t*)n)->elseb, indent, maxdepth);
    }
    break;

  case EXPR_FOR:
    if (maxdepth == 1) {
      abuf_str(s, "for");
    } else {
      forexpr_t* e = (forexpr_t*)n;
      abuf_str(s, "for ");
      if (e->start || e->end) {
        if (e->start)
          fmt(s, (node_t*)e->start, indent, maxdepth - 1);
        abuf_str(s, "; ");
        fmt(s, (node_t*)e->cond, indent, maxdepth - 1);
        abuf_str(s, "; ");
        if (e->end)
          fmt(s, (node_t*)e->start, indent, maxdepth - 1);
      } else {
        fmt(s, (node_t*)e->cond, indent, maxdepth - 1);
      }
      abuf_c(s, ' ');
      fmt(s, (node_t*)e->body, indent, maxdepth - 1);
    }
    break;

  case EXPR_ID:
    abuf_str(s, ((idexpr_t*)n)->name);
    break;

  case EXPR_RETURN:
    abuf_str(s, "return");
    if (((const retexpr_t*)n)->value) {
      abuf_c(s, ' ');
      fmt(s, (node_t*)((const retexpr_t*)n)->value, indent, maxdepth);
    }
    break;

  case EXPR_DEREF:
  case EXPR_PREFIXOP:
    abuf_str(s, tok_repr(((unaryop_t*)n)->op));
    fmt(s, (node_t*)((unaryop_t*)n)->expr, indent, maxdepth);
    break;

  case EXPR_POSTFIXOP:
    fmt(s, (node_t*)((unaryop_t*)n)->expr, indent, maxdepth);
    abuf_str(s, tok_repr(((unaryop_t*)n)->op));
    break;

  case EXPR_BINOP:
    fmt(s, (node_t*)((binop_t*)n)->left, indent, maxdepth - 1);
    abuf_c(s, ' ');
    abuf_str(s, tok_repr(((binop_t*)n)->op));
    abuf_c(s, ' ');
    fmt(s, (node_t*)((binop_t*)n)->right, indent, maxdepth - 1);
    break;

  case EXPR_BOOLLIT:
    abuf_str(s, ((const boollit_t*)n)->val ? "true" : "false");
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
  case TYPE_FUN:  return funtype(s, (const funtype_t*)n, indent, maxdepth);
  case TYPE_STRUCT: return structtype(s, (const structtype_t*)n, indent, maxdepth);
  case TYPE_ARRAY: {
    arraytype_t* a = (arraytype_t*)n;
    abuf_fmt(s, "[%zu]", a->size);
    fmt(s, (node_t*)a->elem, indent, maxdepth);
    break;
  }
  case TYPE_PTR: {
    const ptrtype_t* pt = (const ptrtype_t*)n;
    abuf_c(s, '*');
    fmt(s, (node_t*)pt->elem, indent, maxdepth);
    break;
  }
  case TYPE_REF: {
    const reftype_t* pt = (const reftype_t*)n;
    abuf_str(s, pt->ismut ? "mut&" : "&");
    fmt(s, (node_t*)pt->elem, indent, maxdepth);
    break;
  }
  case TYPE_OPTIONAL: {
    abuf_c(s, '?');
    fmt(s, (node_t*)((const opttype_t*)n)->elem, indent, maxdepth);
    break;
  }

  case TYPE_ENUM:
  case EXPR_FIELD:
    dlog("TODO %s", nodekind_name(n->kind));
    abuf_str(s, "/* TODO fmt ");
    abuf_str(s, nodekind_name(n->kind));
    abuf_str(s, "*/");
    break;

  case NODE_BAD:
  case NODE_COMMENT:
  case NODEKIND_COUNT:
    assertf(0, "unexpected node %s", nodekind_name(n->kind));
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
