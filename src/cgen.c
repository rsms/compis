#include "c0lib.h"
#include "compiler.h"


void cgen_init(cgen_t* g, compiler_t* c, memalloc_t out_ma) {
  memset(g, 0, sizeof(*g));
  g->compiler = c;
  buf_init(&g->outbuf, out_ma);
}


void cgen_dispose(cgen_t* g) {
  buf_dispose(&g->outbuf);
}


static void seterr(cgen_t* g, err_t err) {
  if (!g->err)
    g->err = err;
}


#define error(g, node_or_type, fmt, args...) \
  _error((g), (srcrange_t){ .focus = (node_or_type)->loc }, "[cgen] " fmt, ##args)

ATTR_FORMAT(printf,3,4)
static void _error(cgen_t* g, srcrange_t srcrange, const char* fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  report_errorv(g->compiler, srcrange, fmt, ap);
  va_end(ap);
  seterr(g, ErrInvalid);
}


#define OUT_PUTC(ch) ( \
  buf_push(&g->outbuf, (ch)) ?: seterr(g, ErrNoMem) )

#define OUT_PRINT(cstr) ( \
  buf_print(&g->outbuf, (cstr)) ?: seterr(g, ErrNoMem) )

#define OUT_PRINTF(fmt, args...) ( \
  buf_printf(&g->outbuf, (fmt), ##args) ?: seterr(g, ErrNoMem) )


static void type(cgen_t* g, const type_t* t);
static void stmt(cgen_t* g, const node_t* n);
static void expr(cgen_t* g, const node_t* n);


static void type(cgen_t* g, const type_t* t) {
  switch (t->kind) {
  case TYPE_VOID: OUT_PRINT("void"); break;
  case TYPE_BOOL: OUT_PRINT("_Bool"); break;
  case TYPE_INT:  OUT_PRINT(t->isunsigned ? "unsigned int" : "int"); break;
  case TYPE_I8:   OUT_PRINT(t->isunsigned ? "uint8_t"  : "int8_t"); break;
  case TYPE_I16:  OUT_PRINT(t->isunsigned ? "uint16_t" : "int16_t"); break;
  case TYPE_I32:  OUT_PRINT(t->isunsigned ? "uint32_t" : "int32_t"); break;
  case TYPE_I64:  OUT_PRINT(t->isunsigned ? "uint64_t" : "int64_t"); break;
  case TYPE_F32:  OUT_PRINT("float"); break;
  case TYPE_F64:  OUT_PRINT("double"); break;
  default:
    error(g, t, "unexpected type %s", type_name(t));
  }
}


typedef enum {
  BLOCKFLAG_NONE,
  BLOCKFLAG_RET,
} blockflag_t;


static void stmt_block(cgen_t* g, const node_t* n, blockflag_t fl) {
  OUT_PRINT("{\n");
  if (n->children.len) for (usize i = 0, end = n->children.len - 1; i <= end; i++) {
    if (i == end && (fl & BLOCKFLAG_RET))
      OUT_PRINT("return ");
    stmt(g, n->children.v[i]);
  }
  OUT_PRINT("}\n");
}


static void fun(cgen_t* g, const node_t* n) {
  type(g, n->fun.result_type);
  OUT_PUTC(' ');
  if (n->fun.name == NULL) {
    OUT_PRINTF("_anonfun_%u", g->anon_idgen++);
  } else {
    OUT_PRINT(n->fun.name->strval);
  }
  OUT_PRINT("()");
  if (n->fun.body == NULL) {
    OUT_PRINT(";\n");
  } else if (n->fun.body->kind == EXPR_BLOCK) {
    blockflag_t fl = 0;
    if (n->fun.result_type != type_void)
      fl |= BLOCKFLAG_RET; // return last expression
    stmt_block(g, n->fun.body, fl);
  } else {
    OUT_PRINT("{\n");
    expr(g, n->fun.body);
    OUT_PRINT("}\n");
  }
}


static const char* operator(tok_t tok) {
  switch (tok) {
  case TCOMMA: return ",";

  case TASSIGN:    return "=";
  case TMULASSIGN: return "*=";
  case TDIVASSIGN: return "/=";
  case TMODASSIGN: return "%=";
  case TADDASSIGN: return "+=";
  case TSUBASSIGN: return "-=";
  case TSHLASSIGN: return "<<=";
  case TSHRASSIGN: return ">>=";
  case TANDASSIGN: return "&=";
  case TXORASSIGN: return "^=";
  case TORASSIGN:  return "|=";

  case TEQ:   return "==";
  case TNEQ:  return "!=";

  case TLT:   return "<";
  case TGT:   return ">";
  case TLTEQ: return "<=";
  case TGTEQ: return ">=";

  case TPLUS:       return "+";
  case TPLUSPLUS:   return "++";
  case TMINUS:      return "-";
  case TMINUSMINUS: return "--";
  case TSTAR:       return "*";
  case TSLASH:      return "/";
  case TPERCENT:    return "%";
  case TTILDE:      return "~";
  case TNOT:        return "!";
  case TAND:        return "&";
  case TANDAND:     return "&&";
  case TOR:         return "|";
  case TOROR:       return "||";
  case TXOR:        return "^";
  case TSHL:        return "<<";
  case TSHR:        return ">>";

  default: assertf(0,"bad op %u", tok); return "?";
  }
}


static void infixop(cgen_t* g, const node_t* n) {
  OUT_PUTC('(');
  expr(g, n->op2.left);
  OUT_PRINT(operator(n->op2.op));
  expr(g, n->op2.right);
  OUT_PUTC(')');
}


static void intlit(cgen_t* g, const node_t* n) {
  OUT_PRINTF("%llu", n->intval);
}


static void expr(cgen_t* g, const node_t* n) {
  switch (n->kind) {
  case EXPR_FUN:     return fun(g, n);
  case EXPR_INFIXOP: return infixop(g, n);
  case EXPR_INTLIT:  return intlit(g, n);
  //case EXPR_BLOCK:   return expr_block(g, n);
  default:
    error(g, n, "unexpected node %s", node_name(n));
  }
}


static void stmt(cgen_t* g, const node_t* n) {
  expr(g, n);
  if (n->kind != EXPR_FUN)
    OUT_PRINT(";\n");
}


static void unit(cgen_t* g, const node_t* n) {
  for (usize i = 0; i < n->children.len; i++)
    stmt(g, n->children.v[i]);
}


err_t cgen_generate(cgen_t* g, const node_t* n) {
  // reset generator state
  g->err = 0;
  buf_clear(&g->outbuf);
  g->anon_idgen = 0;

  if (n->kind != NODE_UNIT)
    return ErrInvalid;
  unit(g, n);
  return g->err;
}
