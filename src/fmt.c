// SPDX-License-Identifier: Apache-2.0
#include "colib.h"
#include "compiler.h"


const char* nodekind_fmt(nodekind_t kind) {
  switch (kind) {
    case STMT_TYPEDEF: return "type definition";

    case EXPR_PARAM:                                          return "parameter";
    case EXPR_LET:                                            return "binding";
    case EXPR_VAR:                                            return "variable";
    case EXPR_FUN:                                            return "function";
    case EXPR_BLOCK:                                          return "block";
    case EXPR_ID:                                             return "identifier";
    case EXPR_PREFIXOP: case EXPR_POSTFIXOP: case EXPR_BINOP: return "operation";
    case EXPR_ASSIGN:                                         return "assignment";
    case EXPR_DEREF:                                          return "dereference";
    case EXPR_INTLIT: case EXPR_FLOATLIT: case EXPR_BOOLLIT:  return "constant";
    case EXPR_MEMBER:                                         return "member";
    case EXPR_SUBSCRIPT:                                      return "subscript";
    case EXPR_FIELD:                                          return "field";
    case EXPR_CALL:                                           return "call";

    case TYPE_UNKNOWN:                   return "unknown type";
    case TYPE_ARRAY:                     return "array type";
    case TYPE_FUN:                       return "function type";
    case TYPE_PTR:                       return "pointer type";
    case TYPE_REF: case TYPE_MUTREF:     return "reference type";
    case TYPE_SLICE: case TYPE_MUTSLICE: return "slice type";
    case TYPE_OPTIONAL:                  return "optional type";
    case TYPE_STRUCT:                    return "struct type";
    case TYPE_ALIAS:                     return "alias type";
    case TYPE_TEMPLATE:                  return "template type";
    case TYPE_UNRESOLVED:                return "named type";

    default:
      if (nodekind_istype(kind)) return "type";
      if (nodekind_isexpr(kind)) return "expression";
      return nodekind_name(kind);
  }
}


#define FMT_PARAMS           buf_t* outbuf, u32 indent, u32 maxdepth, u32 templatenest
#define FMT_ARGS             outbuf,            indent,     maxdepth,     templatenest
#define FMT_ARGSD(maxdepth)  outbuf,            indent,     maxdepth,     templatenest

#define CHAR(ch)             buf_push(outbuf, (ch))
#define PRINT(cstr)          buf_print(outbuf, (cstr))
#define PRINTF(fmt, args...) buf_printf(outbuf, (fmt), ##args)
#define PRINTN(bytes, len)   buf_append(outbuf, (bytes), (len))
#define STARTLINE()          startline(outbuf, indent)



static void startline(buf_t* outbuf, u32 indent) {
  if (outbuf->len)
    CHAR('\n');
  buf_fill(outbuf, ' ', (usize)indent * 2);
}


static void fmt(FMT_PARAMS, const node_t* nullable n);


static void local(FMT_PARAMS, const local_t* nullable n) {
  PRINT(n->name);
  CHAR(' ');
  fmt(FMT_ARGS, (node_t*)n->type);
  if (n->init && maxdepth > 1) {
    PRINT(" = ");
    fmt(FMT_ARGS, (node_t*)n->init);
  }
}


static void funtype(FMT_PARAMS, const funtype_t* nullable n) {
  assert(maxdepth > 0);
  CHAR('(');
  for (u32 i = 0; i < n->params.len; i++) {
    if (i) PRINT(", ");
    const local_t* param = (local_t*)n->params.v[i];
    PRINT(param->name);
    if (i+1 == n->params.len || ((local_t*)n->params.v[i+1])->type != param->type) {
      CHAR(' ');
      fmt(FMT_ARGS, (const node_t*)param->type);
    }
  }
  PRINT(") ");
  fmt(FMT_ARGS, (const node_t*)n->result);
}


static void structtype(FMT_PARAMS, const structtype_t* nullable t) {
  if (t->name) {
    PRINT(t->name);
  } else if (maxdepth <= 1) {
    PRINT("struct");
  }

  if (t->flags & (NF_TEMPLATE | NF_TEMPLATEI) && templatenest == 0) {
    CHAR('<');
    u32 maxdepth0 = maxdepth;
    if (maxdepth == 0)
      maxdepth = 1;
    for (u32 i = 0; i < t->templateparams.len; i++) {
      if (i)
        PRINT(", ");
      fmt(FMT_ARGS, (node_t*)t->templateparams.v[i]);
    }
    maxdepth = maxdepth0;
    CHAR('>');
  }

  if (maxdepth <= 1)
    return;

  if (t->name)
    CHAR(' ');
  CHAR('{');
  if (t->fields.len > 0) {
    indent++;
    for (u32 i = 0; i < t->fields.len; i++) {
      STARTLINE();
      const local_t* f = (local_t*)t->fields.v[i];
      PRINT(f->name), CHAR(' ');
      fmt(FMT_ARGS, (const node_t*)f->type);
      if (f->init) {
        PRINT(" = ");
        fmt(FMT_ARGS, (const node_t*)f->init);
      }
    }
    indent--;
    STARTLINE();
  }
  CHAR('}');
}


static void templateparam(FMT_PARAMS, const templateparam_t* tparam) {
  PRINT(tparam->name);
  if (tparam->init && maxdepth > 1) {
    PRINT(" = ");
    fmt(FMT_ARGS, tparam->init);
  }
}


static void fmt_nodearray(FMT_PARAMS, const nodearray_t* nodes, const char* sep) {
  for (u32 i = 0; i < nodes->len; i++) {
    if (i) PRINT(sep);
    fmt(FMT_ARGS, nodes->v[i]);
  }
}


// parenthesize returns true if x should be surrounded by "(...)" when
// used as a value in another expression that might be ambiguous,
// for example "!(x && y)" = (PREFIXOP "!" (BINOP (ID x) (ID y))).
//
// TODO: generalize this for operator precedence so we can use it everywhere
//
static bool parenthesize(const expr_t* x) {
  switch (x->kind) {
  case EXPR_VAR:
  case EXPR_LET:
  case EXPR_BINOP:
    return true;
  // case EXPR_FUN:
  // case EXPR_BLOCK:
  // case EXPR_CALL:
  // case EXPR_TYPECONS:
  // case EXPR_ID:
  // case EXPR_NS:
  // case EXPR_FIELD:
  // case EXPR_PARAM:
  // case EXPR_MEMBER:
  // case EXPR_SUBSCRIPT:
  // case EXPR_PREFIXOP:
  // case EXPR_POSTFIXOP:
  // case EXPR_DEREF:
  // case EXPR_ASSIGN:
  // case EXPR_IF:
  // case EXPR_FOR:
  // case EXPR_RETURN:
  // case EXPR_BOOLLIT:
  // case EXPR_INTLIT:
  // case EXPR_FLOATLIT:
  // case EXPR_STRLIT:
  // case EXPR_ARRAYLIT:
  default:
    return false;
  }
}


static void fmt(FMT_PARAMS, const node_t* nullable n) {
  if (maxdepth == 0)
    return;

  if (!n) {
    PRINT("(NULL)");
    return;
  }

  switch ((enum nodekind)n->kind) {

  case NODE_UNIT: {
    const nodearray_t* a = &((unit_t*)n)->children;
    for (u32 i = 0; i < a->len; i++) {
      STARTLINE();
      fmt(FMT_ARGSD(maxdepth - 1), a->v[i]);
    }
    break;
  }

  case NODE_TPLPARAM:
    templateparam(FMT_ARGS, (templateparam_t*)n);
    break;

  case STMT_IMPORT:
    PRINT("/*TODO import_t*/");
    break;

  case STMT_TYPEDEF:
    if (n->flags & NF_VIS_PUB)
      PRINT("pub ");
    PRINT("type ");
    fmt(FMT_ARGS, (node_t*)((typedef_t*)n)->type);
    break;

  case EXPR_VAR:
  case EXPR_LET:
    PRINT(n->kind == EXPR_VAR ? "var " : "let ");
    FALLTHROUGH;
  case EXPR_PARAM:
  case EXPR_FIELD:
    return local(FMT_ARGS, (const local_t*)n);

  case EXPR_NS: {
    const nsexpr_t* ns = (nsexpr_t*)n;
    if (ns->flags & NF_PKGNS) {
      PRINTF("package \"%s\"", ns->pkg ? ns->pkg->path.p : "?");
    } else {
      PRINT("/*TODO nsexpr_t*/");
    }
    break;
  }

  case EXPR_FUN: {
    if (n->flags & NF_VIS_PUB)
      PRINT("pub ");
    fun_t* fn = (fun_t*)n;
    funtype_t* ft = (funtype_t*)assertnotnull(fn->type);
    assert_nodekind(ft, TYPE_FUN);
    PRINTF("fun %s(", fn->name);
    fmt_nodearray(FMT_ARGS, &ft->params, ", ");
    PRINT(") ");
    fmt(FMT_ARGS, (node_t*)ft->result);
    if (fn->body && maxdepth > 1) {
      CHAR(' ');
      fmt(FMT_ARGSD(maxdepth - 1), (node_t*)fn->body);
    }
    break;
  }

  case EXPR_BLOCK: {
    CHAR('{');
    const nodearray_t* a = &((block_t*)n)->children;
    if (a->len > 0) {
      if (maxdepth <= 1) {
        PRINT("...");
      } else {
        indent++;
        for (u32 i = 0; i < a->len; i++) {
          STARTLINE();
          fmt(FMT_ARGSD(maxdepth - 1), a->v[i]);
        }
        indent--;
        STARTLINE();
      }
    }
    CHAR('}');
    break;
  }

  case EXPR_CALL: {
    const call_t* call = (const call_t*)n;
    fmt(FMT_ARGS, (const node_t*)call->recv);
    CHAR('(');
    fmt_nodearray(FMT_ARGS, &call->args, ", ");
    CHAR(')');
    break;
  }

  case EXPR_TYPECONS: {
    const typecons_t* tc = (const typecons_t*)n;
    fmt(FMT_ARGS, (const node_t*)tc->type);
    CHAR('(');
    fmt(FMT_ARGS, (const node_t*)tc->expr);
    CHAR(')');
    break;
  }

  case EXPR_MEMBER:
    fmt(FMT_ARGS, (node_t*)((const member_t*)n)->recv);
    CHAR('.');
    PRINT(((const member_t*)n)->name);
    break;

  case EXPR_SUBSCRIPT:
    fmt(FMT_ARGS, (node_t*)((const subscript_t*)n)->recv);
    CHAR('[');
    fmt(FMT_ARGS, (node_t*)((const subscript_t*)n)->index);
    CHAR(']');
    break;

  case EXPR_IF:
    PRINT("if ");
    fmt(FMT_ARGS, (node_t*)((const ifexpr_t*)n)->cond);
    CHAR(' ');
    fmt(FMT_ARGS, (node_t*)((const ifexpr_t*)n)->thenb);
    if (((const ifexpr_t*)n)->elseb) {
      PRINT(" else ");
      fmt(FMT_ARGS, (node_t*)((const ifexpr_t*)n)->elseb);
    }
    break;

  case EXPR_FOR:
    if (maxdepth <= 1) {
      PRINT("for");
    } else {
      forexpr_t* e = (forexpr_t*)n;
      PRINT("for ");
      if (e->start || e->end) {
        if (e->start)
          fmt(FMT_ARGSD(maxdepth - 1), (node_t*)e->start);
        PRINT("; ");
        fmt(FMT_ARGSD(maxdepth - 1), (node_t*)e->cond);
        PRINT("; ");
        if (e->end)
          fmt(FMT_ARGSD(maxdepth - 1), (node_t*)e->start);
      } else {
        fmt(FMT_ARGSD(maxdepth - 1), (node_t*)e->cond);
      }
      CHAR(' ');
      fmt(FMT_ARGSD(maxdepth - 1), (node_t*)e->body);
    }
    break;

  case EXPR_ID:
    PRINT(((idexpr_t*)n)->name);
    break;

  case EXPR_RETURN:
    PRINT("return");
    if (((const retexpr_t*)n)->value) {
      CHAR(' ');
      fmt(FMT_ARGS, (node_t*)((const retexpr_t*)n)->value);
    }
    break;

  case EXPR_DEREF:
  case EXPR_PREFIXOP: {
    const unaryop_t* op = (unaryop_t*)n;
    bool group = parenthesize(op->expr);
    #ifdef DEBUG
      const char* opstr = op_fmt(op->op);
      if (*opstr) {
        PRINT(opstr);
      } else {
        PRINTF("«%s»", op_name(op->op)+strlen("OP_"));
        group = true;
      }
    #else
      PRINT(op_fmt(op->op));
    #endif
    if (group) CHAR('(');
    fmt(FMT_ARGS, (node_t*)op->expr);
    if (group) CHAR(')');
    break;
  }

  case EXPR_POSTFIXOP: {
    const unaryop_t* op = (unaryop_t*)n;
    bool group = parenthesize(op->expr);
    if (group) CHAR('(');
    fmt(FMT_ARGS, (node_t*)op->expr);
    if (group) CHAR(')');
    PRINT(op_fmt(op->op));
    break;
  }

  case EXPR_ASSIGN:
  case EXPR_BINOP:
    fmt(FMT_ARGS, (node_t*)((binop_t*)n)->left);
    CHAR(' ');
    PRINT(op_fmt(((binop_t*)n)->op));
    CHAR(' ');
    fmt(FMT_ARGS, (node_t*)((binop_t*)n)->right);
    break;

  case EXPR_BOOLLIT:
    PRINT(((intlit_t*)n)->intval ? "true" : "false");
    break;

  case EXPR_INTLIT:
    buf_print_u64(outbuf, ((intlit_t*)n)->intval, 10);
    break;

  case EXPR_FLOATLIT:
    buf_print_f64(outbuf, ((floatlit_t*)n)->f64val, -1);
    break;

  case EXPR_STRLIT: {
    const strlit_t* str = (strlit_t*)n;
    CHAR('"');
    buf_appendrepr(outbuf, str->bytes, str->len);
    CHAR('"');
    break;
  }

  case EXPR_ARRAYLIT:
    CHAR('[');
    if (maxdepth <= 1) {
      PRINT("...");
    } else {
      fmt_nodearray(FMT_ARGS, &((arraylit_t*)n)->values, ", ");
    }
    CHAR(']');
    break;

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
    PRINT(primtype_name(n->kind));
    break;

  case TYPE_STRUCT:
    return structtype(FMT_ARGS, (const structtype_t*)n);

  case TYPE_FUN:
    PRINT("fun");
    return funtype(FMT_ARGS, (const funtype_t*)n);

  case TYPE_ARRAY: {
    arraytype_t* t = (arraytype_t*)n;
    CHAR('[');
    fmt(FMT_ARGS, (node_t*)t->elem);
    if (t->len > 0) {
      PRINTF(" %llu", t->len);
    } else if (t->lenexpr) {
      CHAR(' ');
      fmt(FMT_ARGS, (node_t*)t->lenexpr);
    }
    CHAR(']');
    break;
  }

  case TYPE_SLICE:
  case TYPE_MUTSLICE: {
    slicetype_t* t = (slicetype_t*)n;
    PRINT(n->kind == TYPE_MUTSLICE ? "mut&[" : "&[");
    fmt(FMT_ARGS, (node_t*)t->elem);
    CHAR(']');
    break;
  }

  case TYPE_PTR: {
    const ptrtype_t* pt = (const ptrtype_t*)n;
    CHAR('*');
    fmt(FMT_ARGS, (node_t*)pt->elem);
    break;
  }

  case TYPE_REF:
  case TYPE_MUTREF: {
    const reftype_t* pt = (const reftype_t*)n;
    PRINT(n->kind == TYPE_MUTREF ? "mut&" : "&");
    fmt(FMT_ARGS, (node_t*)pt->elem);
    break;
  }

  case TYPE_OPTIONAL: {
    CHAR('?');
    fmt(FMT_ARGS, (node_t*)((const opttype_t*)n)->elem);
    break;

  case TYPE_NS:
    PRINT("namespace");
    break;
  }

  case TYPE_ALIAS: {
    const aliastype_t* at = (aliastype_t*)n;
    PRINT(at->name);
    if (maxdepth > 1) {
      CHAR(' ');
      fmt(FMT_ARGS, (node_t*)at->elem);
    }
    break;
  }

  case TYPE_TEMPLATE: {
    const templatetype_t* tt = (templatetype_t*)n;
    templatenest++;
    fmt(FMT_ARGS, (node_t*)tt->recv);
    templatenest--;
    CHAR('<');
    fmt_nodearray(FMT_ARGS, &tt->args, ", ");
    CHAR('>');
    break;
  }

  case TYPE_PLACEHOLDER: {
    const placeholdertype_t* pt = (placeholdertype_t*)n;
    templateparam(FMT_ARGS, pt->templateparam);
    break;
  }

  case TYPE_UNKNOWN:
    PRINT("unknown");
    break;

  case TYPE_UNRESOLVED:
    PRINT(((unresolvedtype_t*)n)->name);
    break;

  case NODE_BAD:
  case NODE_IMPORTID:
  case NODE_COMMENT:
  case NODE_FWDDECL:
    PRINTF("/*%s*/", nodekind_name(n->kind));
    break;

  } // switch
}


err_t node_fmt(buf_t* outbuf, const node_t* n, u32 maxdepth) {
  if (outbuf->oom)
    return ErrNoMem;

  u32 indent = 0;
  u32 templatenest = 0;
  if (maxdepth == 0)
    maxdepth = 1;

  fmt(FMT_ARGS, n);

  buf_nullterm(outbuf);

  bool oom = outbuf->oom;
  outbuf->oom = false;

  return oom ? ErrNoMem : 0;
}


const char* fmtnode(u32 bufindex, const void* nullable n) {
  buf_t* buf = tmpbuf_get(bufindex);
  err_t err = node_fmt(buf, n, /*depth*/0);
  safecheck(err == 0);
  return buf->chars;
}
