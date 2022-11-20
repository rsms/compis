#include "c0lib.h"
#include "compiler.h"


bool cgen_init(cgen_t* g, compiler_t* c, memalloc_t out_ma) {
  memset(g, 0, sizeof(*g));
  g->compiler = c;
  buf_init(&g->outbuf, out_ma);
  if (!map_init(&g->tmpmap, g->compiler->ma, 32))
    return false;
  return true;
}


void cgen_dispose(cgen_t* g) {
  buf_dispose(&g->outbuf);
  map_dispose(&g->tmpmap, g->compiler->ma);
}


static void seterr(cgen_t* g, err_t err) {
  if (!g->err)
    g->err = err;
}


#define error(g, node_or_type, fmt, args...) \
  _error((g), (srcrange_t){ .focus = assertnotnull(node_or_type)->loc }, \
    "[cgen] " fmt, ##args)

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


#define CHAR(ch) ( \
  buf_push(&g->outbuf, (ch)) ?: seterr(g, ErrNoMem) )

#define PRINT(cstr) ( \
  buf_print(&g->outbuf, (cstr)) ?: seterr(g, ErrNoMem) )

#define PRINTF(fmt, args...) ( \
  buf_printf(&g->outbuf, (fmt), ##args) ?: seterr(g, ErrNoMem) )


static void startline(cgen_t* g, srcloc_t loc) {
  g->lineno++;
  if ((loc.line != 0) & ((g->lineno != loc.line) | (g->input != loc.input))) {
    g->lineno = loc.line;
    PRINTF("\n#line %u", g->lineno);
    if (g->input != loc.input) {
      g->input = loc.input;
      PRINTF(" \"%s\"", g->input->name);
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


static void structtype(cgen_t* g, const structtype_t* n) {
  if (n->name && g->scopenest > 0)
    return PRINT(n->name);
  PRINT("struct {");
  if (n->fields.len == 0) {
    PRINT("u8 _unused;");
  } else {
    g->indent++;
    const type_t* t = NULL;
    for (u32 i = 0; i < n->fields.len; i++) {
      const local_t* f = n->fields.v[i];
      bool newline = f->loc.line != g->lineno;
      if (newline) {
        if (i) CHAR(';');
        t = NULL;
        startline(g, f->loc);
      }
      if (f->type != t) {
        if (i && !newline) PRINT("; ");
        type(g, f->type);
        CHAR(' ');
        t = f->type;
      } else {
        PRINT(", ");
      }
      PRINT(f->name);
    }
    CHAR(';');
    g->indent--;
    startline(g, (srcloc_t){0});
  }
  CHAR('}');
}


static void type(cgen_t* g, const type_t* t) {
  switch (t->kind) {
  case TYPE_VOID: PRINT("void"); break;
  case TYPE_BOOL: PRINT("_Bool"); break;
  case TYPE_INT:  PRINT(t->isunsigned ? "unsigned int" : "int"); break;
  case TYPE_I8:   PRINT(t->isunsigned ? "uint8_t"  : "int8_t"); break;
  case TYPE_I16:  PRINT(t->isunsigned ? "uint16_t" : "int16_t"); break;
  case TYPE_I32:  PRINT(t->isunsigned ? "uint32_t" : "int32_t"); break;
  case TYPE_I64:  PRINT(t->isunsigned ? "uint64_t" : "int64_t"); break;
  case TYPE_F32:  PRINT("float"); break;
  case TYPE_F64:  PRINT("double"); break;
  case TYPE_STRUCT: return structtype(g, (const structtype_t*)t);
  default: error(g, t, "unexpected type %s", nodekind_name(t->kind));
  }
}


static void zeroinit(cgen_t* g, const type_t* t) {
  switch (t->kind) {
  case TYPE_BOOL:
    PRINT("false");
    break;
  case TYPE_INT:
  case TYPE_I32:
    PRINT(t->isunsigned ? "0u" : "0");
    break;
  case TYPE_I8:
  case TYPE_I16:
  case TYPE_I64:
    CHAR('('); CHAR('('); type(g, t); PRINT(")0)");
    break;
  case TYPE_F32:
    PRINT("0.0f");
    break;
  case TYPE_F64:
    PRINT("0.0");
    break;
  default:
    error(g, t, "unexpected type %s", nodekind_name(t->kind));
  }
  // PRINTF(";memset(&%s,0,%zu)", n->name, n->type->size);
}


typedef enum {
  BLOCKFLAG_NONE,
  BLOCKFLAG_RET,
  BLOCKFLAG_EXPR,
} blockflag_t;


static void block(cgen_t* g, const block_t* n, blockflag_t fl) {
  u32 hasval = n->type != type_void && (fl & BLOCKFLAG_EXPR);
  g->scopenest++;

  if (hasval) {
    type(g, n->type);
    CHAR(' ');
    PRINTF("_block_%zx", (uintptr)n);
    if (hasval && n->children.len == 1) {
      PRINT(" = ");
      expr(g, n->children.v[0]);
      g->scopenest--;
      return;
    }
    CHAR(';');
  }

  CHAR('{');
  if (n->children.len > 0) {
    g->indent++;
    for (u32 i = 0, last = n->children.len - 1; i < n->children.len; i++) {
      const expr_t* cn = n->children.v[i];
      startline(g, cn->loc);
      if (i == last) {
        if (hasval) {
          PRINTF("_block_%zx = ", (uintptr)n);
        } else if (fl & BLOCKFLAG_RET) {
          PRINT("return ");
        }
      }
      assertf(nodekind_isexpr(cn->kind), "%s", nodekind_name(cn->kind));
      expr(g, (const expr_t*)cn);
      PRINT(";");
    }
    g->indent--;
    startline(g, (srcloc_t){0});
  }
  CHAR('}');
  g->scopenest--;
}


static void structinit(cgen_t* g, const structtype_t* t, const ptrarray_t* args) {
  CHAR('{');
  u32 i = 0;
  for (; i < args->len; i++) {
    const node_t* arg = args->v[i];
    if (arg->kind == EXPR_PARAM)
      break;
    if (i) PRINT(", ");
    expr(g, (const expr_t*)arg);
  }
  if (i == args->len) {
    CHAR('}');
    return;
  }

  // named arguments
  u32 posend = i;

  // record fields with non-zero initializers
  map_t* initmap = &g->tmpmap;
  map_clear(initmap);
  for (u32 i = posend; i < t->fields.len; i++) {
    const local_t* f = t->fields.v[i];
    if (f->init) {
      const void** vp = (const void**)map_assign_ptr(initmap, g->compiler->ma, f->name);
      if UNLIKELY(!vp)
        return seterr(g, ErrNoMem);
      *vp = f;
    }
  }

  // generate named arguments
  for (; i < args->len; i++) {
    if (i) PRINT(", ");
    const local_t* arg = args->v[i];
    CHAR('.'); PRINT(arg->name); CHAR('=');
    expr(g, assertnotnull(arg->init));
    map_del_ptr(initmap, arg->name);
  }

  // generate remaining fields with non-zero initializers
  for (const mapent_t* e = map_it(initmap); map_itnext(initmap, &e); ) {
    if (i) PRINT(", ");
    const local_t* f = e->value;
    CHAR('.'); PRINT(f->name); CHAR('=');
    expr(g, assertnotnull(f->init));
  }

  CHAR('}');
}


static void typecall(cgen_t* g, const call_t* n, const type_t* t) {
  // skip redundant "(T)v" when v is T
  if (type_isprim(t)) {
    assert(n->args.len == 1);
    const expr_t* arg = n->args.v[0];
    if (arg->type == t)
      return expr(g, arg);
  }

  CHAR('('); type(g, t); CHAR(')');

  switch (t->kind) {
  case TYPE_VOID: PRINT("((void)0)"); break;
  case TYPE_BOOL:
  case TYPE_INT:
  case TYPE_I8:
  case TYPE_I16:
  case TYPE_I32:
  case TYPE_I64:
  case TYPE_F32:
  case TYPE_F64:
    if (n->args.len == 0) {
      zeroinit(g, t);
    } else {
      assert(n->args.len == 1);
      expr(g, n->args.v[0]);
    }
    break;
  case TYPE_STRUCT:
    structinit(g, (const structtype_t*)t, &n->args);
    break;
  default:
    dlog("NOT IMPLEMENTED: type call %s", nodekind_name(t->kind));
    error(g, t, "NOT IMPLEMENTED: type call %s", nodekind_name(t->kind));
  }
}


static void call(cgen_t* g, const call_t* n) {
  // type call?
  const idexpr_t* idrecv = (const idexpr_t*)n->recv;
  if (n->recv->kind == EXPR_ID && nodekind_istype(idrecv->ref->kind))
    return typecall(g, n, (const type_t*)idrecv->ref);
  if (nodekind_istype(n->recv->kind))
    return typecall(g, n, (const type_t*)n->recv);

  expr(g, n->recv);
  CHAR('(');
  for (u32 i = 0; i < n->args.len; i++) {
    if (i) PRINT(", ");
    const expr_t* arg = n->args.v[i];
    if (arg->kind == EXPR_PARAM) // named argument
      arg = ((const local_t*)arg)->init;
    expr(g, arg);
  }
  CHAR(')');
}


static void id(cgen_t* g, sym_t nullable name) {
  if (name && name != sym__) {
    PRINT(name);
  } else {
    PRINTF("_anon%u", g->anon_idgen++);
  }
}


static void fun(cgen_t* g, const fun_t* fun) {
  type(g, ((funtype_t*)fun->type)->result);
  CHAR(' ');
  id(g, fun->name);
  CHAR('(');
  if (fun->params.len > 0) {
    for (u32 i = 0; i < fun->params.len; i++) {
      local_t* param = fun->params.v[i];
      if (i) PRINT(", ");
      type(g, param->type);
      CHAR(' ');
      id(g, param->name);
    }
  } else {
    PRINT("void");
  }
  CHAR(')');
  if (fun->body == NULL) {
    PRINT(";\n");
  } else if (fun->body->kind == EXPR_BLOCK) {
    blockflag_t fl = 0;
    if (((funtype_t*)fun->type)->result != type_void)
      fl |= BLOCKFLAG_RET; // return last expression
    CHAR(' ');
    block(g, (block_t*)fun->body, fl);
  } else {
    PRINT(" { return ");
    expr(g, fun->body);
    PRINT("}\n");
  }
}


static void binop(cgen_t* g, const binop_t* n) {
  if (!tok_isassign(n->op))
    CHAR('(');
  expr(g, n->left);
  CHAR(' ');
  PRINT(operator(n->op));
  CHAR(' ');
  expr(g, n->right);
  if (!tok_isassign(n->op))
    CHAR(')');
}


static void intlit(cgen_t* g, const intlit_t* n) {
  if (n->type->kind < TYPE_I32)
    CHAR('('), type(g, n->type), CHAR(')');

  u64 u = n->intval;
  if (!n->type->isunsigned && (u & 0x1000000000000000) ) {
    u &= ~0x1000000000000000;
    CHAR('-');
  }
  u32 base = u >= 1024 ? 16 : 10;
  if (base == 16)
    PRINT("0x");
  buf_print_u64(&g->outbuf, u, base) ?: seterr(g, ErrNoMem);

  if (n->type->kind > TYPE_I32)
    PRINT("ll");
  if (n->type->isunsigned)
    CHAR('u');
}


static void floatlit(cgen_t* g, const floatlit_t* n) {
  if (n->type->kind == TYPE_F64) {
    PRINTF("%f", n->f64val);
  } else {
    PRINTF("%ff", n->f32val);
  }
}


static void idexpr(cgen_t* g, const idexpr_t* n) {
  id(g, n->name);
}


static void member(cgen_t* g, const member_t* n) {
  CHAR('('); expr(g, n->recv); CHAR(')');
  PRINT(type_isusertype(n->recv->type) ? "->" : ".");
  PRINT(n->name);
}


static void vardef(cgen_t* g, const local_t* n) {
  type(g, n->type);
  CHAR(' ');
  if (n->name == sym__)
    PRINT("__attribute__((__unused__)) ");
  id(g, n->name);
  PRINT(" = ");
  if (n->init) {
    expr(g, n->init);
  } else {
    zeroinit(g, n->type);
  }
}


static void letdef(cgen_t* g, const local_t* n) {
  PRINT("const ");
  type(g, n->type);
  CHAR(' ');
  if (n->name == sym__)
    PRINT("__attribute__((__unused__)) ");
  id(g, n->name);
  PRINT(" = ");
  assertnotnull(n->init);
  expr(g, n->init);
}


static void typdef(cgen_t* g, const typedef_t* n) {
  PRINT("typedef ");
  type(g, n->type);
  CHAR(' ');
  id(g, n->name);
}


static void expr(cgen_t* g, const expr_t* n) {
  switch ((enum nodekind)n->kind) {
  case EXPR_FUN:      return fun(g, (const fun_t*)n);
  case EXPR_BINOP:    return binop(g, (const binop_t*)n);
  case EXPR_INTLIT:   return intlit(g, (const intlit_t*)n);
  case EXPR_FLOATLIT: return floatlit(g, (const floatlit_t*)n);
  case EXPR_ID:       return idexpr(g, (const idexpr_t*)n);
  // case EXPR_PARAM: TODO
  case EXPR_VAR:    return vardef(g, (const local_t*)n);
  case EXPR_LET:    return letdef(g, (const local_t*)n);
  case EXPR_BLOCK:  return block(g, (const block_t*)n, BLOCKFLAG_EXPR);
  case EXPR_CALL:   return call(g, (const call_t*)n);
  case EXPR_MEMBER: return member(g, (const member_t*)n);
  // case EXPR_PREFIXOP: TODO
  // case EXPR_POSTFIXOP: TODO

  // node types we should never see
  case NODEKIND_COUNT:
  case NODE_BAD:
  case NODE_COMMENT:
  case NODE_UNIT:
  case NODE_FIELD:
  case STMT_TYPEDEF:
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
  case TYPE_STRUCT:
    break;
  }
  debugdie(g, n, "unexpected node %s", nodekind_name(n->kind));
}


static void stmt(cgen_t* g, const stmt_t* n) {
  bool semi = true;
  startline(g, n->loc);
  switch (n->kind) {
  case EXPR_FUN:
    fun(g, (const fun_t*)n); semi = false;
    break;
  case STMT_TYPEDEF:
    typdef(g, (const typedef_t*)n);
    break;
  default:
    if (nodekind_isexpr(n->kind)) {
      expr(g, (expr_t*)n);
      break;
    }
    debugdie(g, n, "unexpected stmt node %s", nodekind_name(n->kind));
  }
  if (semi)
    PRINT(";\n");
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
  g->scopenest = 0;

  PRINT("#include <stdint.h>\n");
  PRINT("#define true 1\n");
  PRINT("#define false 0\n");

  if (n->kind != NODE_UNIT)
    return ErrInvalid;
  unit(g, n);

  CHAR('\n');

  return g->err;
}
