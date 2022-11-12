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


static void type(cgen_t* g, const type_t*);
static void stmt(cgen_t* g, const stmt_t*);
static void expr(cgen_t* g, const expr_t*);


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
    error(g, t, "unexpected type %s", nodekind_name(t->kind));
  }
}


typedef enum {
  BLOCKFLAG_NONE,
  BLOCKFLAG_RET,
} blockflag_t;


static void stmt_block(cgen_t* g, const block_t* block, blockflag_t fl) {
  OUT_PRINT("{\n");
  if (block->children.len) {
    for (usize i = 0, end = block->children.len - 1; i <= end; i++) {
      if (i == end && (fl & BLOCKFLAG_RET))
        OUT_PRINT("return ");
      stmt(g, block->children.v[i]);
    }
  }
  OUT_PRINT("}\n");
}


static void fun(cgen_t* g, const fun_t* fun) {
  type(g, fun->result_type);
  OUT_PUTC(' ');
  if (fun->name) {
    OUT_PRINT(fun->name->sym);
  } else {
    OUT_PRINTF("_anonfun_%u", g->anon_idgen++);
  }
  OUT_PUTC('(');
  for (usize i = 0; i < fun->params.len; i++) {
    local_t* param = fun->params.v[i];
    if (i) OUT_PUTC(',');
    type(g, param->type);
    OUT_PUTC(' ');
    OUT_PRINT(param->name);
  }
  OUT_PUTC(')');
  if (fun->body == NULL) {
    OUT_PRINT(";\n");
  } else if (fun->body->kind == EXPR_BLOCK) {
    blockflag_t fl = 0;
    if (fun->result_type != type_void)
      fl |= BLOCKFLAG_RET; // return last expression
    stmt_block(g, (block_t*)fun->body, fl);
  } else {
    OUT_PRINT("{\n");
    expr(g, fun->body);
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


static void infixop(cgen_t* g, const op2expr_t* n) {
  OUT_PUTC('(');
  expr(g, n->left);
  OUT_PRINT(operator(n->op));
  expr(g, n->right);
  OUT_PUTC(')');
}


static void intlit(cgen_t* g, const intlitexpr_t* n) {
  OUT_PRINTF("%llu", n->intval);
}


static void expr(cgen_t* g, const expr_t* n) {
  switch (n->kind) {
  case EXPR_FUN:     return fun(g, (const fun_t*)n);
  case EXPR_INFIXOP: return infixop(g, (const op2expr_t*)n);
  case EXPR_INTLIT:  return intlit(g, (const intlitexpr_t*)n);
  //case EXPR_BLOCK:   return expr_block(g, n);
  default:
    error(g, n, "unexpected node %s", nodekind_name(n->kind));
  }
}


static void stmt(cgen_t* g, const stmt_t* n) {
  bool semi = true;
  switch (n->kind) {
  case EXPR_FUN:
    fun(g, (const fun_t*)n); semi = false; break;

  case EXPR_BLOCK:
  case EXPR_ID:
  case EXPR_PREFIXOP:
  case EXPR_POSTFIXOP:
  case EXPR_INFIXOP:
  case EXPR_INTLIT:
    expr(g, (expr_t*)n); break;

  default:
    error(g, n, "unexpected stmt node %s", nodekind_name(n->kind));
  }
  if (semi)
    OUT_PRINT(";\n");
}


static void unit(cgen_t* g, const unit_t* n) {
  for (usize i = 0; i < n->children.len; i++)
    stmt(g, n->children.v[i]);
}


err_t cgen_generate(cgen_t* g, const unit_t* n) {
  // reset generator state
  g->err = 0;
  buf_clear(&g->outbuf);
  g->anon_idgen = 0;
  if (n->kind != NODE_UNIT)
    return ErrInvalid;
  unit(g, n);
  return g->err;
}
