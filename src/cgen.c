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

#if DEBUG
  #define debugdie(g, node_or_type, fmt, args...) ( \
    _error((g), (srcrange_t){ .focus = (node_or_type)->loc }, "[cgen] " fmt, ##args), \
    panic("code generator got unexpected AST") \
  )
#else
  #define debugdie(...) ((void)0)
#endif

ATTR_FORMAT(printf,3,4)
static void _error(cgen_t* g, srcrange_t srcrange, const char* fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  report_diagv(g->compiler, srcrange, DIAG_ERR, fmt, ap);
  va_end(ap);
  seterr(g, ErrInvalid);
}


#define OUT_PUTC(ch) ( \
  buf_push(&g->outbuf, (ch)) ?: seterr(g, ErrNoMem) )

#define OUT_PRINT(cstr) ( \
  buf_print(&g->outbuf, (cstr)) ?: seterr(g, ErrNoMem) )

#define OUT_PRINTF(fmt, args...) ( \
  buf_printf(&g->outbuf, (fmt), ##args) ?: seterr(g, ErrNoMem) )


static void startline(cgen_t* g, srcloc_t loc) {
  g->lineno++;
  if ((loc.line != 0) & ((g->lineno != loc.line) | (g->input != loc.input))) {
    g->lineno = loc.line;
    OUT_PRINTF("\n#line %u", g->lineno);
    if (g->input != loc.input) {
      g->input = loc.input;
      OUT_PRINTF(" \"%s\"", g->input->name);
    }
  }
  u8* p = buf_alloc(&g->outbuf, g->indent*2 + 1);
  if UNLIKELY(!p) {
    seterr(g, ErrNoMem);
    return;
  }
  *p = '\n';
  memset(p+1, ' ', g->indent*2);
}


static void type(cgen_t* g, const type_t*);
static void stmt(cgen_t* g, const stmt_t*);
static void expr(cgen_t* g, const expr_t*);


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
  default: error(g, t, "unexpected type %s", nodekind_name(t->kind));
  }
}


typedef enum {
  BLOCKFLAG_NONE,
  BLOCKFLAG_RET,
  BLOCKFLAG_EXPR,
} blockflag_t;


static void block(cgen_t* g, const block_t* n, blockflag_t fl) {
  u32 hasval = n->type != type_void && (fl & BLOCKFLAG_EXPR);

  if (hasval) {
    type(g, n->type);
    OUT_PUTC(' ');
    OUT_PRINTF("_block_%zx", (uintptr)n);
    if (hasval && n->children.len == 1) {
      OUT_PRINT(" = ");
      expr(g, n->children.v[0]);
      return;
    }
    OUT_PUTC(';');
  }

  OUT_PUTC('{');
  if (n->children.len > 0) {
    g->indent++;
    for (u32 i = 0, last = n->children.len - 1; i < n->children.len; i++) {
      const expr_t* cn = n->children.v[i];
      startline(g, cn->loc);
      if (i == last) {
        if (hasval) {
          OUT_PRINTF("_block_%zx = ", (uintptr)n);
        } else if (fl & BLOCKFLAG_RET) {
          OUT_PRINT("return ");
        }
      }
      assertf(nodekind_isexpr(cn->kind), "%s", nodekind_name(cn->kind));
      expr(g, (const expr_t*)cn);
      OUT_PRINT(";");
    }
    g->indent--;
    startline(g, (srcloc_t){0});
  }
  OUT_PUTC('}');
}


static void id(cgen_t* g, sym_t nullable name) {
  if (name && name != sym__) {
    OUT_PRINT(name);
  } else {
    OUT_PRINTF("_anon%u", g->anon_idgen++);
  }
}


static void fun(cgen_t* g, const fun_t* fun) {
  startline(g, fun->loc);
  type(g, ((funtype_t*)fun->type)->result);
  OUT_PUTC(' ');
  id(g, fun->name);
  OUT_PUTC('(');
  if (fun->params.len > 0) {
    for (u32 i = 0; i < fun->params.len; i++) {
      local_t* param = fun->params.v[i];
      if (i) OUT_PRINT(", ");
      type(g, param->type);
      OUT_PUTC(' ');
      id(g, param->name);
    }
  } else {
    OUT_PRINT("void");
  }
  OUT_PUTC(')');
  if (fun->body == NULL) {
    OUT_PRINT(";\n");
  } else if (fun->body->kind == EXPR_BLOCK) {
    blockflag_t fl = 0;
    if (((funtype_t*)fun->type)->result != type_void)
      fl |= BLOCKFLAG_RET; // return last expression
    OUT_PUTC(' ');
    block(g, (block_t*)fun->body, fl);
  } else {
    OUT_PRINT(" { return ");
    expr(g, fun->body);
    OUT_PRINT("}\n");
  }
}


static void binop(cgen_t* g, const binop_t* n) {
  if (!tok_isassign(n->op))
    OUT_PUTC('(');
  expr(g, n->left);
  OUT_PUTC(' ');
  OUT_PRINT(operator(n->op));
  OUT_PUTC(' ');
  expr(g, n->right);
  if (!tok_isassign(n->op))
    OUT_PUTC(')');
}


static void intlit(cgen_t* g, const intlit_t* n) {
  OUT_PRINTF("%llu", n->intval);
}


static void idexpr(cgen_t* g, const idexpr_t* n) {
  id(g, n->name);
}


static void zeroinit(cgen_t* g, const type_t* t) {
  switch (t->kind) {
  case TYPE_BOOL:
    OUT_PRINT("false");
    break;
  case TYPE_INT:
  case TYPE_I32:
    OUT_PRINT(t->isunsigned ? "0u" : "0");
    break;
  case TYPE_I8:
  case TYPE_I16:
  case TYPE_I64:
    OUT_PUTC('('); OUT_PUTC('('); type(g, t); OUT_PRINT(")0)");
    break;
  case TYPE_F32:
    OUT_PRINT("0.0f");
    break;
  case TYPE_F64:
    OUT_PRINT("0.0");
    break;
  default:
    error(g, t, "unexpected type %s", nodekind_name(t->kind));
  }
  // OUT_PRINTF(";memset(&%s,0,%zu)", n->name, n->type->size);
}


static void vardef(cgen_t* g, const local_t* n) {
  type(g, n->type);
  OUT_PUTC(' ');
  if (n->name == sym__)
    OUT_PRINT("__attribute__((__unused__)) ");
  id(g, n->name);
  OUT_PRINT(" = ");
  if (n->init) {
    expr(g, n->init);
  } else {
    zeroinit(g, n->type);
  }
}


static void letdef(cgen_t* g, const local_t* n) {
  OUT_PRINT("const ");
  type(g, n->type);
  OUT_PUTC(' ');
  if (n->name == sym__)
    OUT_PRINT("__attribute__((__unused__)) ");
  id(g, n->name);
  OUT_PRINT(" = ");
  assertnotnull(n->init);
  expr(g, n->init);
}


static void expr(cgen_t* g, const expr_t* n) {
  switch (n->kind) {
  case EXPR_FUN:    return fun(g, (const fun_t*)n);
  case EXPR_BINOP:  return binop(g, (const binop_t*)n);
  case EXPR_INTLIT: return intlit(g, (const intlit_t*)n);
  case EXPR_ID:     return idexpr(g, (const idexpr_t*)n);
  case EXPR_VAR:    return vardef(g, (const local_t*)n);
  case EXPR_LET:    return letdef(g, (const local_t*)n);
  case EXPR_BLOCK:  return block(g, (const block_t*)n, BLOCKFLAG_EXPR);
  }
  debugdie(g, n, "unexpected node %s", nodekind_name(n->kind));
}


static void stmt(cgen_t* g, const stmt_t* n) {
  bool semi = true;
  switch (n->kind) {
  case EXPR_FUN:
    fun(g, (const fun_t*)n); semi = false; break;

  default:
    if (nodekind_isexpr(n->kind)) {
      expr(g, (expr_t*)n);
      break;
    }
    debugdie(g, n, "unexpected stmt node %s", nodekind_name(n->kind));
  }
  if (semi)
    OUT_PRINT(";\n");
}


static void unit(cgen_t* g, const unit_t* n) {
  for (u32 i = 0; i < n->children.len; i++)
    stmt(g, n->children.v[i]);
}


err_t cgen_generate(cgen_t* g, const unit_t* n) {
  // reset generator state
  g->err = 0;
  buf_clear(&g->outbuf);
  g->anon_idgen = 0;
  g->input = NULL;
  g->lineno = 0;

  OUT_PRINT("#include <stdint.h>\n");

  if (n->kind != NODE_UNIT)
    return ErrInvalid;
  unit(g, n);

  OUT_PUTC('\n');

  return g->err;
}
