// SPDX-License-Identifier: Apache-2.0
#include "c0lib.h"
#include "compiler.h"


typedef struct { usize v[2]; } sizetuple_t;


bool cgen_init(cgen_t* g, compiler_t* c, memalloc_t out_ma) {
  memset(g, 0, sizeof(*g));
  g->compiler = c;
  buf_init(&g->outbuf, out_ma);
  buf_init(&g->headbuf, out_ma);
  if (!map_init(&g->typedefmap, g->compiler->ma, 32))
    return false;
  if (!map_init(&g->tmpmap, g->compiler->ma, 32)) {
    map_dispose(&g->typedefmap, g->compiler->ma);
    return false;
  }
  return true;
}


void cgen_dispose(cgen_t* g) {
  map_dispose(&g->tmpmap, g->compiler->ma);
  map_dispose(&g->typedefmap, g->compiler->ma);
  buf_dispose(&g->headbuf);
  buf_dispose(&g->outbuf);
}


inline static locmap_t* locmap(cgen_t* g) {
  return &g->compiler->locmap;
}


static void seterr(cgen_t* g, err_t err) {
  if (!g->err)
    g->err = err;
}


#define error(g, node_or_type, fmt, args...) \
  _error((g), origin_make(locmap(g), assertnotnull(node_or_type)->loc), \
    "[cgen] " fmt, ##args)

ATTR_FORMAT(printf,3,4)
static void _error(cgen_t* g, origin_t origin, const char* fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  report_diagv(g->compiler, origin, DIAG_ERR, fmt, ap);
  va_end(ap);
  seterr(g, ErrInvalid);
}


#if DEBUG
  #define debugdie(g, node_or_type, fmt, args...) ( \
    error((g), (node_or_type), fmt, ##args), \
    panic("code generator got unexpected AST") \
  )
#else
  #define debugdie(...) ((void)0)
#endif


#define INTERNAL_PREFIX "_c0·"
#define ANON_PREFIX     "_·"
#define ANON_FMT        ANON_PREFIX "%x"

#define CHAR(ch)             ( buf_push(&g->outbuf, (ch)), ((void)0) )
#define PRINT(cstr)          ( buf_print(&g->outbuf, (cstr)), ((void)0) )
#define PRINTF(fmt, args...) ( buf_printf(&g->outbuf, (fmt), ##args), ((void)0) )


static char lastchar(cgen_t* g) {
  assert(g->outbuf.len > 0);
  return g->outbuf.chars[g->outbuf.len-1];
}


static bool startloc(cgen_t* g, loc_t loc) {
  bool inputok = loc_inputid(loc) == 0 || g->inputid == loc_inputid(loc);
  u32 lineno = loc_line(loc);

  if (lineno == 0 || (g->lineno == lineno && inputok))
    return false;

  if (g->lineno < lineno && inputok && lineno - g->lineno < 4) {
    buf_fill(&g->outbuf, '\n', lineno - g->lineno);
  } else {
    if (g->outbuf.len && g->outbuf.chars[g->outbuf.len-1] != '\n')
      CHAR('\n');
    if (g->scopenest == 0)
      CHAR('\n');
    PRINTF("#line %u", lineno);
    if (!inputok) {
      input_t* input = loc_input(loc, locmap(g));
      g->inputid = loc_inputid(loc);
      PRINTF(" \"%s\"", assertnotnull(input)->name);
    }
  }

  g->lineno = lineno;
  return true;
}


static void startline(cgen_t* g, loc_t loc) {
  g->lineno++;
  startloc(g, loc);
  CHAR('\n');
  buf_fill(&g->outbuf, ' ', g->indent*2);
}


static void startlinex(cgen_t* g) {
  g->lineno++;
  CHAR('\n');
  buf_fill(&g->outbuf, ' ', g->indent*2);
}


static sizetuple_t x_semi_begin_char(cgen_t* g, char c) {
  CHAR(c);
  return (sizetuple_t){{ g->outbuf.len - 1, g->outbuf.len }};
}


static sizetuple_t x_semi_begin_startline(cgen_t* g, loc_t loc) {
  usize len0 = g->outbuf.len;
  startline(g, loc);
  return (sizetuple_t){{ len0, g->outbuf.len }};
}


static void x_semi_cancel(sizetuple_t* startlens) {
  startlens->v[1] = 0;
}


static void x_semi_end(cgen_t* g, sizetuple_t startlens) {
  if (g->outbuf.len == startlens.v[1]) {
    // undo startline
    g->outbuf.len = startlens.v[0];
  } else if (startlens.v[1] != 0) {
    CHAR(';');
  }
}


static void type(cgen_t* g, const type_t*);
static void stmt(cgen_t* g, const stmt_t*);
static void expr(cgen_t* g, const expr_t*);
static void expr_as_value(cgen_t* g, const expr_t* n);


static const char* operator(op_t op) {
  switch ((enum op)op) {
  case OP_ALIAS:
  case OP_ARG:
  case OP_BORROW:
  case OP_BORROW_MUT:
  case OP_CALL:
  case OP_DROP:
  case OP_FCONST:
  case OP_FUN:
  case OP_ICONST:
  case OP_MOVE:
  case OP_NOOP:
  case OP_OCHECK:
  case OP_PHI:
  case OP_STORE:
  case OP_VAR:
  case OP_ZERO:
    break;

  // unary
  case OP_INC: return "++";
  case OP_DEC: return "--";
  case OP_INV: return "~";
  case OP_NOT: return "!";
  case OP_DEREF:  return "*";

  // binary, arithmetic
  case OP_ADD: return "+";
  case OP_SUB: return "-";
  case OP_MUL: return "*";
  case OP_DIV: return "/";
  case OP_MOD: return "%";

  // binary, bitwise
  case OP_AND: return "&";
  case OP_OR:  return "|";
  case OP_XOR: return "^";
  case OP_SHL: return "<<";
  case OP_SHR: return ">>";

  // binary, logical
  case OP_LAND: return "&&";
  case OP_LOR:  return "||";

  // binary, comparison
  case OP_EQ:   return "==";
  case OP_NEQ:  return "!=";
  case OP_LT:   return "<";
  case OP_GT:   return ">";
  case OP_LTEQ: return "<=";
  case OP_GTEQ: return ">=";

  // binary, assignment
  case OP_ASSIGN:     return "=";
  case OP_ADD_ASSIGN: return "+=";
  case OP_AND_ASSIGN: return "&=";
  case OP_DIV_ASSIGN: return "/=";
  case OP_MOD_ASSIGN: return "%=";
  case OP_MUL_ASSIGN: return "*=";
  case OP_OR_ASSIGN:  return "|=";
  case OP_SHL_ASSIGN: return "<<=";
  case OP_SHR_ASSIGN: return ">>=";
  case OP_SUB_ASSIGN: return "-=";
  case OP_XOR_ASSIGN: return "^=";
  }
  assertf(0,"bad op %u", op);
  return "?";
}


static bool type_is_interned_def(const type_t* t) {
  return t->kind == TYPE_STRUCT
      || t->kind == TYPE_OPTIONAL
      || t->kind == TYPE_ALIAS
      ;
}


typedef sym_t(*gentypename_t)(cgen_t* g, const type_t* t);
typedef void(*gentypedef_t)(cgen_t* g, const type_t* t, sym_t name);


static sym_t intern_typedef(
  cgen_t* g, const type_t* t, gentypename_t gentypename, gentypedef_t gentypedef)
{
  assertf(type_is_interned_def(t), "update type_is_interned_def");
  const void* key = t;
  if (t->kind == TYPE_OPTIONAL)
    key = typeid((type_t*)t);

  sym_t* vp = (sym_t*)map_assign_ptr(&g->typedefmap, g->compiler->ma, key);
  if UNLIKELY(!vp)
    return seterr(g, ErrNoMem), sym__;
  if (*vp)
    return *vp;

  buf_t headbuf = g->headbuf;
  usize headoffs = g->headoffs;
  u32 lineno = g->lineno;
  g->lineno = 0;

  if (g->headnest) {
    // allocate buffer for potential nested call to intern_typedef by f
    g->headbuf = buf_make(g->compiler->ma);
    headbuf.len = 0;
    g->headoffs = 0;
  }

  // save & replace outbuf
  buf_t outbuf = g->outbuf;
  g->outbuf = headbuf;
  g->headnest++;

  if (startloc(g, t->loc))
    CHAR('\n');

  sym_t name = gentypename(g, t);
  *vp = name;

  gentypedef(g, t, name);
  PRINT("\n");

  // restore outbuf
  g->headnest--;
  headbuf = g->outbuf; // reload
  g->outbuf = outbuf;

  // in case this is a nested call to intern_typedef, insert into parent headbuf
  if (g->headnest) {
    if (headoffs > 0 && g->outbuf.chars[headoffs - 1] != '\n') {
      CHAR('\n');
      headoffs++;
    }
    if (!buf_insert(&g->outbuf, headoffs, headbuf.p, headbuf.len))
      seterr(g, ErrNoMem);
    g->headoffs = g->outbuf.len;
    buf_dispose(&g->headbuf);
  } else if (g->lineno != lineno) {
    // // write "#line N" if it is not already in outbuf
    // char tmp[24];
    // usize len = snprintf(tmp, sizeof(tmp), "\n#line %u /*x*/\n", lineno);
    // if (g->outbuf.len <= len ||
    //   g->outbuf.chars[g->outbuf.len - len - 1] != '\n' ||
    //   memcmp(&g->outbuf.chars[g->outbuf.len - len], tmp, len) != 0)
    // {
    //   PRINT(tmp);
    // }
    g->lineno = lineno;
  }

  g->headbuf = headbuf;

  return name;
}


static void funtype(cgen_t* g, const funtype_t* t, const char* nullable name) {
  // void(*name)(args)
  type(g, t->result);
  PRINT("(*");
  if (!name || name == sym__) {
    PRINTF(ANON_FMT, g->anon_idgen++);
  } else {
    PRINT(name);
  }
  PRINT(")(");
  if (t->params.len == 0) {
    PRINT("void");
  } else {
    for (u32 i = 0; i < t->params.len; i++) {
      local_t* param = t->params.v[i]; assert(param->kind == EXPR_PARAM);
      // if (!type_isprim(param->type) && !param->ismut)
      //   PRINT("const ");
      if (i) PRINT(", ");
      type(g, param->type);
      if (param->name && param->name != sym__) {
        CHAR(' ');
        dlog(">> %s", param->name);
        PRINT(param->name);
      }
    }
  }
  CHAR(')');
}


static void fun(cgen_t* g, const fun_t* fun);


static sym_t gen_struct_typename(cgen_t* g, const type_t* t) {
  const structtype_t* st = (const structtype_t*)t;
  if (st->name)
    return st->name;
  char buf[strlen(ANON_PREFIX "structXXXXXXXX.")];
  return sym_snprintf(buf, sizeof(buf), ANON_PREFIX "struct%x", g->anon_idgen++);
}


static void gen_struct_typedef(cgen_t* g, const type_t* tp, sym_t typename) {
  const structtype_t* n = (const structtype_t*)tp;
  PRINT("typedef struct {");
  if (n->fields.len == 0) {
    PRINT("u8 _unused;");
  } else {
    g->indent++;
    const type_t* t = NULL;
    for (u32 i = 0; i < n->fields.len; i++) {
      const local_t* f = n->fields.v[i];
      bool newline = loc_line(f->loc) != g->lineno;
      if (newline) {
        if (i) CHAR(';');
        t = NULL;
        startline(g, f->loc);
      }
      if (f->type != t) {
        if (i && !newline) PRINT("; ");
        if (f->type->kind == TYPE_FUN) {
          funtype(g, (const funtype_t*)f->type, f->name);
          continue;
        }
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
    startline(g, (loc_t){0});
  }
  PRINT("} ");
  PRINT(typename);
  CHAR(';');
}


static void structtype(cgen_t* g, const structtype_t* t) {
  PRINT(intern_typedef(g, (const type_t*)t, gen_struct_typename, gen_struct_typedef));
}


static void reftype(cgen_t* g, const reftype_t* t) {
  if (!t->ismut)
    PRINT("const ");
  type(g, t->elem);
  CHAR('*');
}


static void ptrtype(cgen_t* g, const ptrtype_t* t) {
  type(g, t->elem);
  CHAR('*');
}


static sym_t gen_alias_typename(cgen_t* g, const type_t* t) {
  return ((aliastype_t*)t)->name;
}

static void gen_alias_typedef(cgen_t* g, const type_t* tp, sym_t typename) {
  const aliastype_t* t = (const aliastype_t*)tp;
  PRINT("typedef "); type(g, t->elem); PRINTF(" %s;", typename);
}

static void aliastype(cgen_t* g, const aliastype_t* t) {
  PRINT(intern_typedef(g, (type_t*)t, gen_alias_typename, gen_alias_typedef));
}


static sym_t gen_opt_typename(cgen_t* g, const type_t* t) {
  char namebuf[64];
  return sym_snprintf(namebuf, sizeof(namebuf), ANON_PREFIX "opt%x", g->anon_idgen++);
}

static void gen_opt_typedef(cgen_t* g, const type_t* tp, sym_t typename) {
  const opttype_t* t = (const opttype_t*)tp;
  PRINT("typedef struct{bool ok; "); type(g, t->elem); PRINTF(" v;} %s;", typename);
}

static void opttype(cgen_t* g, const opttype_t* t) {
  if (type_isptrlike(t->elem)) {
    // NULL used for "no value"
    type(g, t->elem);
  } else {
    assert(t->elem->kind != TYPE_OPTIONAL);
    PRINT(intern_typedef(g, (type_t*)t, gen_opt_typename, gen_opt_typedef));
  }
}


static void optinit(cgen_t* g, const expr_t* init, bool isshort) {
  if (type_isptrlike(init->type))
    return expr_as_value(g, init);
  assert(init->type->kind != TYPE_OPTIONAL);
  if (!isshort) {
    opttype_t t = { .kind = TYPE_OPTIONAL, .elem = (type_t*)init->type };
    CHAR('('), opttype(g, &t), CHAR(')');
  }
  PRINT("{true,"); expr_as_value(g, init); CHAR('}');
}


static void optzero(cgen_t* g, const type_t* elem, bool isshort) {
  assert(elem->kind != TYPE_OPTIONAL);
  if (type_isptrlike(elem)) {
    PRINT("NULL");
  } else {
    if (!isshort) {
      opttype_t t = { .kind = TYPE_OPTIONAL, .elem = (type_t*)elem };
      CHAR('('), opttype(g, &t), CHAR(')');
    }
    PRINT("{0}");
  }
}


static void type(cgen_t* g, const type_t* t) {
  switch (t->kind) {
  case TYPE_VOID: PRINT("void"); break;
  case TYPE_BOOL: PRINT("bool"); break;
  case TYPE_I8:   PRINT(t->isunsigned ? "uint8_t"  : "int8_t"); break;
  case TYPE_I16:  PRINT(t->isunsigned ? "uint16_t" : "int16_t"); break;
  case TYPE_I32:  PRINT(t->isunsigned ? "uint32_t" : "int32_t"); break;
  case TYPE_I64:  PRINT(t->isunsigned ? "uint64_t" : "int64_t"); break;
  case TYPE_INT:  PRINT(t->isunsigned ? "unsigned int" : "int"); break;
  case TYPE_F32:  PRINT("float"); break;
  case TYPE_F64:  PRINT("double"); break;
  case TYPE_FUN:      return funtype(g, (const funtype_t*)t, NULL);
  case TYPE_PTR:      return ptrtype(g, (const ptrtype_t*)t);
  case TYPE_REF:      return reftype(g, (const reftype_t*)t);
  case TYPE_OPTIONAL: return opttype(g, (const opttype_t*)t);
  case TYPE_STRUCT:   return structtype(g, (const structtype_t*)t);
  case TYPE_ALIAS:    return aliastype(g, (const aliastype_t*)t);
  case TYPE_ARRAY:    panic("TODO %s", nodekind_name(t->kind));

  default:
    panic("unexpected type_t %s (%u)", nodekind_name(t->kind), t->kind);
  }
}


static void expr_as_value(cgen_t* g, const expr_t* n) {
  switch (n->kind) {
  case EXPR_INTLIT:
  case EXPR_FLOATLIT:
  case EXPR_ID:
  case EXPR_PARAM:
  case EXPR_PREFIXOP:
  case EXPR_POSTFIXOP:
  case EXPR_MEMBER:
  case EXPR_BLOCK:
  case EXPR_CALL:
    return expr(g, n);
  default:
    CHAR('('); expr(g, n); CHAR(')');
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
  case TYPE_OPTIONAL:
    PRINT("{0}");
    break;
  case TYPE_PTR:
    PRINT("NULL");
    break;
  default:
    debugdie(g, t, "unexpected type %s", nodekind_name(t->kind));
  }
  // PRINTF(";memset(&%s,0,%zu)", n->name, n->type->size);
}


static void expr_or_zeroinit(cgen_t* g, const type_t* t, const expr_t* nullable n) {
  if (n) {
    assert(types_iscompat(g->compiler, t, n->type));
    return expr(g, n);
  }
  zeroinit(g, t);
}


typedef enum {
  BLOCKFLAG_NONE,
  BLOCKFLAG_RET,
} blockflag_t;


#define ID_SIZE(key) (sizeof(ANON_PREFIX key) + sizeof(uintptr)*8 + 1)
#define TMP_ID_SIZE  ID_SIZE("tmp")

#define FMT_ID(buf, bufcap, key, ptr) ( \
  assert((bufcap) >= ID_SIZE(key)), \
  snprintf((buf), (bufcap), ANON_PREFIX key "%zx", (uintptr)(ptr)) )

static usize fmt_tmp_id(char* buf, usize bufcap, const void* n) {
  return FMT_ID(buf, bufcap, "tmp", n);
}

// static void print_tmp_id(cgen_t* g, const void* n) {
//   char tmp[TMP_ID_SIZE];
//   usize len = fmt_tmp_id(tmp, sizeof(tmp), n);
//   buf_append(&g->outbuf, tmp, len);
// }


//—————————————————————————————————————————————————————————————————————————————————————


static void retexpr(cgen_t* g, const retexpr_t* n, const char* nullable tmp) {
  if (tmp) {
    assert(n->type != type_void);
    type(g, n->type), PRINT(" const "), PRINT(tmp);
    if (!n->value)
      return;
    PRINT(" = ");
    if (!n->value) {
      zeroinit(g, n->type);
    } else {
      expr(g, n->value);
    }
  } else if (!n->value) {
    PRINT("return");
  } else {
    PRINT("return "), expr(g, n->value);
  }
}


static void drop_begin(cgen_t* g, const expr_t* owner) {
  PRINT(type_isopt(owner->type) ?
    INTERNAL_PREFIX "drop_opt(" :
    INTERNAL_PREFIX "drop(");
}


static void drop_end(cgen_t* g) {
  CHAR(')');
}


static void cleanups(cgen_t* g, const ptrarray_t* cleanup) {
  for (u32 i = 0; i < cleanup->len; i++) {
    const expr_t* owner = cleanup->v[i];

    if (owner->kind == EXPR_CALL)
      continue;

    assert(nodekind_islocal(owner->kind));
    const local_t* local = (const local_t*)owner;

    startlinex(g);
    drop_begin(g, owner);
    PRINT(local->name);
    drop_end(g);
    CHAR(';');
  }
}


static sizetuple_t x_semi_begin(cgen_t* g, loc_t loc) {
  if (loc_line(loc) != g->lineno && loc_line(loc))
    return x_semi_begin_startline(g, loc);
  return x_semi_begin_char(g, ' ');
}


static void startline_if_needed(cgen_t* g, loc_t loc) {
  if (loc_line(loc) != g->lineno && loc_line(loc))
    startline(g, loc);
}


static bool expr_contains_owners(const expr_t* n) {
  if (type_isowner(n->type))
    return true;
  switch (n->kind) {
    case EXPR_FIELD:
    case EXPR_PARAM:
    case EXPR_VAR:
    case EXPR_LET:
      if (((local_t*)n)->init)
        return expr_contains_owners(((local_t*)n)->init);
      return false;

    case EXPR_ID:
      if (((idexpr_t*)n)->ref && node_isexpr(((idexpr_t*)n)->ref))
        return expr_contains_owners((expr_t*)((idexpr_t*)n)->ref);
      return false;

    case EXPR_RETURN:
      if (((retexpr_t*)n)->value)
        return expr_contains_owners(((retexpr_t*)n)->value);
      return false;

    case EXPR_PREFIXOP:
    case EXPR_POSTFIXOP:
      return expr_contains_owners(((unaryop_t*)n)->expr);

    case EXPR_ASSIGN:
    case EXPR_BINOP:
      return expr_contains_owners(((binop_t*)n)->left) &&
             expr_contains_owners(((binop_t*)n)->right);

    case EXPR_BLOCK: {
      const block_t* b = (const block_t*)n;
      for (u32 i = 0; i < b->children.len; i++) {
        if (expr_contains_owners(b->children.v[i]))
          return true;
      }
      return false;
    }

    case EXPR_CALL: {
      const call_t* call = (const call_t*)n;
      if (expr_contains_owners(call->recv))
        return true;
      for (u32 i = 0; i < call->args.len; i++) {
        if (expr_contains_owners(call->args.v[i]))
          return true;
      }
      return false;
    }

    case EXPR_FUN:
      return false;

    case EXPR_BOOLLIT:
    case EXPR_INTLIT:
    case EXPR_FLOATLIT:
      return false;

    default:
      if (!node_isexpr((const node_t*)n))
        return false;
      // FIXME until we cover all expression types, assume n contains owners
      dlog("TODO %s %s", __FUNCTION__, nodekind_name(n->kind));
      return true;
  }
}


static void cleanups_before_stmt(
  cgen_t* g, const ptrarray_t* cleanup, const void* node)
{
  if (cleanup->len) {
    cleanups(g, cleanup);
    startline(g, ((const node_t*)node)->loc);
  } else {
    startline_if_needed(g, ((const node_t*)node)->loc);
  }
}


static void block(cgen_t* g, const block_t* n, blockflag_t fl) {
  g->scopenest++;

  if (n->flags & NF_RVALUE) {
    if (n->cleanup.len == 0) {
      // simplify empty expression block
      if (n->children.len == 0) {
        PRINT("((void)0)");
        g->scopenest--;
        return;
      }
      // simplify expression block with a single sub expression
      if (n->children.len == 1) {
        expr_as_value(g, n->children.v[0]);
        g->scopenest--;
        return;
      }
    }
    CHAR('(');
  }

  CHAR('{');

  bool block_isrvalue = n->type != type_void && (n->flags & NF_RVALUE);
  char block_resvar[ID_SIZE("block")];
  if (block_isrvalue) {
    FMT_ID(block_resvar, sizeof(block_resvar), "block", n);
    type(g, n->type), CHAR(' '), PRINT(block_resvar), CHAR(';');
  }

  u32 start_lineno = g->lineno;
  g->indent++;
  char tmp[TMP_ID_SIZE];

  if (n->children.len > 0) {
    sizetuple_t startlens;
    for (u32 i = 0, last = n->children.len - 1; i <= last; i++) {
      const expr_t* cn = n->children.v[i];

      // before returning we need to generate cleanups, however the return value
      // might use a local that is cleaned up, so we must generate cleanups _after_
      // the return expression but _before_ returning.
      // To solve this we store the result of the return expression in a temporary.
      // Example:
      //   fun dothing(ref &int) int
      //   fun stuff(x *int) int {
      //     return dothing(x)
      //   }
      // Becomes:
      //   fun dothing(ref &int) int
      //   fun stuff(x *int) int {
      //     int tmp = dothing(x)
      //     drop(x)
      //     return tmp
      //   }
      //

      if (cn->kind == EXPR_RETURN) {
        // explicit return from function (any block)
        // e.g. "fun foo(x int) int { return x }"
        const retexpr_t* ret = (const retexpr_t*)cn;
        if (ret->type == type_void) {
          startline_if_needed(g, cn->loc);
          if (ret->value)
            expr(g, ret->value), CHAR(';');
          cleanups(g, &n->cleanup);
        } else if (n->cleanup.len && expr_contains_owners(cn)) {
          startline_if_needed(g, cn->loc);
          fmt_tmp_id(tmp, sizeof(tmp), ret);
          // "T tmp = expr;"
          type(g, ret->type), PRINT(" const "), PRINT(tmp), PRINT(" = ");
          expr_or_zeroinit(g, ret->type, ret->value), CHAR(';');
          cleanups(g, &n->cleanup);
          startlinex(g);
          PRINT("return "), PRINT(tmp), CHAR(';');
        } else {
          cleanups_before_stmt(g, &n->cleanup, cn);
          retexpr(g, (const retexpr_t*)cn, NULL), CHAR(';');
        }
        break;
      }

      if (i == last) {
        if (fl & BLOCKFLAG_RET) {
          // implicit return from function body block
          // e.g. "fun foo(x int) int { x }"
          assertf(!block_isrvalue, "function block flagged NF_RVALUE");
          x_semi_begin(g, cn->loc);
          if (cn->type == type_void) {
            expr(g, cn), CHAR(';');
            cleanups(g, &n->cleanup);
          } else if (n->cleanup.len && expr_contains_owners(cn)) {
            fmt_tmp_id(tmp, sizeof(tmp), cn);
            // "T tmp = expr;"
            type(g, cn->type), PRINTF(" %s = ", tmp), expr(g, cn), CHAR(';');
            cleanups(g, &n->cleanup);
            startlinex(g);
            PRINT("return "), PRINT(tmp), CHAR(';');
          } else {
            cleanups_before_stmt(g, &n->cleanup, cn);
            PRINT("return "), expr(g, cn), CHAR(';');
          }
          break;
        }
        if (block_isrvalue) {
          // implicit return from non function-body block
          // e.g. "fun foo(x int) int { { x } }"
          x_semi_begin(g, cn->loc);
          PRINT(block_resvar), PRINT(" = "), expr(g, cn), CHAR(';');
          cleanups(g, &n->cleanup);
          break;
        }
      }

      // regular statement or expression
      startlens = x_semi_begin(g, cn->loc);
      expr(g, cn);
      if ((cn->kind == EXPR_BLOCK || cn->kind == EXPR_IF) && lastchar(g) == '}')
        x_semi_cancel(&startlens);
      x_semi_end(g, startlens);

      if (i == last)
        cleanups(g, &n->cleanup);
    } // for
    g->indent--;
    if (start_lineno != g->lineno) {
      startline(g, (loc_t){0});
    } else {
      CHAR(' ');
    }
  } else { // empty block
    if (n->cleanup.len > 0) {
      cleanups(g, &n->cleanup);
      g->indent--;
      startlinex(g);
    } else {
      g->indent--;
    }
  }

  g->scopenest--;

  if (block_isrvalue)
    PRINT(block_resvar), CHAR(';');

  if (n->flags & NF_RVALUE) {
    PRINT("})");
  } else {
    CHAR('}');
  }
}


static void id(cgen_t* g, sym_t nullable name) {
  if (name && name != sym__) {
    PRINT(name);
  } else {
    PRINTF(ANON_FMT, g->anon_idgen++);
  }
}


static sym_t member_recv_name(const type_t* recv) {
  switch (recv->kind) {
    case TYPE_STRUCT:
      if (((structtype_t*)recv)->name)
        return ((structtype_t*)recv)->name;
      break;
  }
  dlog("TODO %s %s", __FUNCTION__, nodekind_name(recv->kind));
  return sym__;
}


static void member_name(cgen_t* g, const type_t* recv, sym_t member) {
  PRINT(member_recv_name(recv));
  PRINT("·"); // U+00B7 MIDDLE DOT (UTF8: "\xC2\xB7")
  PRINT(member);
}


static void fun_name(cgen_t* g, const fun_t* fun) {
  if (fun->methodof) {
    member_name(g, fun->methodof, fun->name);
  } else {
    id(g, fun->name);
  }
}


static void fun_proto(cgen_t* g, const fun_t* fun) {
  funtype_t* ft = (funtype_t*)fun->type;

  type(g, ft->result);
  CHAR(' ');
  fun_name(g, fun);
  CHAR('(');
  if (ft->params.len > 0) {
    g->scopenest++;
    for (u32 i = 0; i < ft->params.len; i++) {
      local_t* param = ft->params.v[i];
      if (i) PRINT(", ");
      // if (!type_isprim(param->type) && !param->ismut)
      //   PRINT("const ");
      type(g, param->type);
      if (param->name && param->name != sym__) {
        CHAR(' ');
        PRINT(param->name);
      }
    }
    g->scopenest--;
  } else {
    PRINT("void");
  }
  CHAR(')');
}


static void fun(cgen_t* g, const fun_t* fun) {
  // if (type_isowner(((funtype_t*)fun->type)->result))
  //   PRINT("__attribute__((__return_typestate__(unconsumed))) ");

  fun_proto(g, fun);

  if (!fun->body)
    return;

  funtype_t* ft = (funtype_t*)fun->type;

  blockflag_t blockflags = 0;
  if (ft->result != type_void)
    blockflags |= BLOCKFLAG_RET; // return last expression

  CHAR(' ');
  block(g, fun->body, blockflags);
}


static void structinit_field(cgen_t* g, const type_t* t, const expr_t* value) {
  if (t->kind == TYPE_OPTIONAL && !type_isptrlike(((const opttype_t*)t)->elem)) {
    PRINT("{.ok=1,.v="); expr(g, value); CHAR('}');
  } else {
    expr(g, value);
  }
}


static void structinit(cgen_t* g, const structtype_t* t, const ptrarray_t* args) {
  assert(args->len <= t->fields.len);
  CHAR('{');
  u32 i = 0;
  for (; i < args->len; i++) {
    const expr_t* arg = args->v[i]; assert(nodekind_isexpr(arg->kind));
    const local_t* f = t->fields.v[0];
    if (arg->kind == EXPR_PARAM)
      break;
    if (i) PRINT(", ");
    structinit_field(g, f->type, arg);
  }

  if (i == args->len && !t->hasinit) {
    if (i == 0 && t->fields.len > 0) {
      const local_t* f = t->fields.v[0];
      zeroinit(g, f->type);
    }
    CHAR('}');
    return;
  }

  // named arguments
  u32 posend = i;

  // create map of fields (name => field)
  map_t* initmap = &g->tmpmap;
  map_clear(initmap);
  for (u32 i = posend; i < t->fields.len; i++) {
    const local_t* f = t->fields.v[i];
    const void** vp = (const void**)map_assign_ptr(initmap, g->compiler->ma, f->name);
    if UNLIKELY(!vp)
      return seterr(g, ErrNoMem);
    *vp = f;
  }

  // generate named arguments
  for (; i < args->len; i++) {
    if (i) PRINT(", ");
    const local_t* arg = args->v[i];
    CHAR('.'); PRINT(arg->name); CHAR('=');
    const local_t** fp = (const local_t**)map_lookup_ptr(initmap, arg->name);
    assert(fp && *fp);
    structinit_field(g, (*fp)->type, assertnotnull(arg->init));
    map_del_ptr(initmap, arg->name);
  }

  // generate remaining fields with non-zero initializers
  // FIXME TODO: sort
  for (const mapent_t* e = map_it(initmap); map_itnext(initmap, &e); ) {
    const local_t* f = e->value;
    if (f->init) {
      if (i) PRINT(", ");
      CHAR('.'); PRINT(f->name); CHAR('=');
      structinit_field(g, (f)->type, assertnotnull(f->init));
    }
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
      expr_as_value(g, n->args.v[0]);
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

  // okay, then it must be a function call
  assert(n->recv->type->kind == TYPE_FUN);

  // owner sink? (i.e. return value is unused but must be dropped)
  bool owner_sink = (n->flags & NF_RVALUE) == 0 && type_isowner(n->type);
  if (owner_sink)
    drop_begin(g, (expr_t*)n);

  // recv
  expr_t* self = NULL;
  bool isselfref = false;
  if (n->recv->kind == EXPR_MEMBER && ((member_t*)n->recv)->target->kind == EXPR_FUN) {
    member_t* m = (member_t*)n->recv;
    fun_t* fn = (fun_t*)m->target;
    funtype_t* ft = (funtype_t*)fn->type;
    if (ft->params.len > 0 && ((const local_t*)ft->params.v[0])->isthis) {
      const local_t* thisparam = ft->params.v[0];
      isselfref = thisparam->type->kind == TYPE_REF;
      self = m->recv;
    }
    assert(fn->name != sym__);
    fun_name(g, fn);
  } else {
    expr(g, n->recv);
  }

  // args
  CHAR('(');
  if (self) {
    if (isselfref && self->type->kind != TYPE_REF)
      CHAR('&');
    expr(g, self);
    if (n->args.len > 0)
      PRINT(", ");
  }
  for (u32 i = 0; i < n->args.len; i++) {
    if (i) PRINT(", ");
    const expr_t* arg = n->args.v[i];
    if (arg->kind == EXPR_PARAM) // named argument
      arg = ((const local_t*)arg)->init;
    expr(g, arg);
  }
  CHAR(')');

  if (owner_sink)
    drop_end(g);
}


static void binop(cgen_t* g, const binop_t* n) {
  expr_as_value(g, n->left);
  CHAR(' ');
  PRINT(operator(n->op));
  CHAR(' ');
  expr_as_value(g, n->right);
}


static void assign(cgen_t* g, const binop_t* n) {
  expr_as_value(g, n->left);
  CHAR(' ');
  PRINT(operator(n->op));
  CHAR(' ');
  if (type_isopt(n->type) && !type_isopt(n->right->type))
    return optinit(g, n->right, /*isshort*/false);
  expr_as_value(g, n->right);
}


static void prefixop(cgen_t* g, const unaryop_t* n) {
  if (n->expr->kind == EXPR_INTLIT && n->expr->type->kind < TYPE_I32)
    CHAR('('), type(g, n->expr->type), CHAR(')');
  PRINT(operator(n->op));
  expr_as_value(g, n->expr);
}


static void postfixop(cgen_t* g, const unaryop_t* n) {
  expr_as_value(g, n->expr);
  PRINT(operator(n->op));
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
  buf_print_u64(&g->outbuf, u, base);

  if (n->type->kind == TYPE_I64)
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


static void boollit(cgen_t* g, const intlit_t* n) {
  PRINT(n->intval ? "true" : "false");
}


static void idexpr(cgen_t* g, const idexpr_t* n) {
  id(g, n->name);
}


static void param(cgen_t* g, const local_t* n) {
  id(g, n->name);
}


static void member(cgen_t* g, const member_t* n) {
  // TODO: nullcheck doesn't work for assignments, e.g. "foo->ptr = ptr"
  // bool insert_nullcheck = n->type->kind == TYPE_REF || n->type->kind == TYPE_FUN;
  bool insert_nullcheck = false;
  if (insert_nullcheck) {
    PRINT("__nullcheck(");
    expr(g, n->recv);
  } else {
    expr_as_value(g, n->recv);
  }
  if (n->recv->type->kind == TYPE_OPTIONAL) {
    panic("TODO optional access!");
  }
  PRINT(n->recv->type->kind == TYPE_REF ? "->" : ".");
  PRINT(n->name);
  if (insert_nullcheck)
    CHAR(')');
}


static void expr_in_block(cgen_t* g, const expr_t* n) {
  if (n->kind == EXPR_BLOCK || n->kind == EXPR_IF)
    return expr(g, n);
  PRINT("{ ");
  expr(g, n);
  PRINT("; }");
}


static void vardef1(cgen_t* g, const local_t* n, const char* name, bool wrap_rvalue) {

  // elide unused variable (unless it has side effects)
  if (n->nrefs == 0 && expr_no_side_effects((expr_t*)n))
    return;

  if ((n->flags & NF_RVALUE) && wrap_rvalue)
    PRINT("({");
  type(g, n->type);
  if (n->kind == EXPR_LET && (type_isprim(n->type) ||
    n->type->kind == TYPE_REF || n->type->kind == TYPE_OPTIONAL))
  {
    PRINT(" const");
  }
  CHAR(' ');

  id(g, name);

  if (n->nrefs == 0)
    PRINT(" __attribute__((__unused__))");

  // if (type_isptr(n->type)) {
  //   PRINTF(" __attribute__((__consumable__(%s)))",
  //     owner_islive(n) ? "unconsumed" : "consumed");
  // }

  PRINT(" = ");
  if (n->init) {
    if (n->type->kind == TYPE_OPTIONAL && n->init->type->kind != TYPE_OPTIONAL) {
      optinit(g, n->init, /*isshort*/true);
      //optinit(g, ((const opttype_t*)n->type)->elem, n->init, /*isshort*/true);
    } else {
      expr(g, n->init);
    }
  } else {
    zeroinit(g, n->type);
  }
  if ((n->flags & NF_RVALUE) && wrap_rvalue)
    PRINT("; "), PRINT(name), PRINT(";})");
}


static void vardef(cgen_t* g, const local_t* n) {
  vardef1(g, n, n->name, true);
}


static void ifexpr(cgen_t* g, const ifexpr_t* n) {
  // TODO: rewrite and clean up this monster of a function
  bool hasvar = n->cond->kind == EXPR_LET || n->cond->kind == EXPR_VAR;
  bool has_tmp_opt = false;
  char tmp[TMP_ID_SIZE];

  if (n->flags & NF_RVALUE)
    g->indent++;

  if (hasvar) {
    // optional check with var assignment
    // e.g. "if let x = optional_x { x }" becomes:
    //   ({ optional0 tmp = varinit; T x = tmp.v; if (tmp.ok) ... })
    // or, when varinit has no side effects:
    //   ({ T x = varinit.v; if (varinit.ok) ... })
    const local_t* var = (const local_t*)n->cond;
    assert(!type_isopt(var->type)); // should be narrowed & have NF_OPTIONAL

    if (n->flags & NF_RVALUE)
      CHAR('(');
    PRINT("{ ");

    g->indent++;

    if ((var->flags & NF_OPTIONAL) == 0 ||
        expr_no_side_effects(var->init) ||
        type_isptrlike(var->type))
    {
      // avoid tmp var when var->init is guaranteed to not have side effects
      startline(g, var->loc);

      // "T x = init;" | "T x = init.v;"
      vardef1(g, var, var->name, false);
      if ((var->flags & NF_OPTIONAL) && !type_isptrlike(var->type))
        PRINT(".v");
      PRINT("; ");

      //   "if (x != NULL)" | "((x != NULL) ?"
      // | "if (init.ok)"   | "((init.ok) ?"
      // | "if (x)"         | "((x) ?"
      if (n->flags & NF_RVALUE) {
        CHAR('(');
      } else {
        PRINT("if ");
      }
      if ((var->flags & NF_OPTIONAL) && !type_isptrlike(var->type)) {
        CHAR('('), expr_as_value(g, var->init), PRINT(".ok)");
      } else {
        PRINTF("(%s)", var->name);
      }
      CHAR(' ');
      if (n->flags & NF_RVALUE)
        CHAR('?');
    } else {
      assert(var->flags & NF_OPTIONAL);
      fmt_tmp_id(tmp, sizeof(tmp), var);

      // "opt0 tmp = init;"
      assert(!type_isopt(var->type));
      opttype_t t = { .kind = TYPE_OPTIONAL, .elem = (type_t*)var->type };
      opttype(g, &t);
      PRINT(" const "), PRINT(tmp), PRINT(" = ");
      expr_as_value(g, var->init);
      CHAR(';');

      // "K x = tmp.v;"
      startline(g, var->loc);
      type(g, var->type);
      if (var->kind == EXPR_LET)
        PRINT(" const");
      PRINTF(" %s = %s.v; ", var->name, tmp);

      //   "if (x != NULL)" | "((x != NULL) ?"
      // | "if (init.ok)"   | "((init.ok) ?"
      if (n->flags & NF_RVALUE) {
        PRINTF("(%s.ok ?", tmp);
      } else {
        PRINTF("if (%s.ok) ", tmp);
      }
    }
  } else {
    // no var definition in conditional
    has_tmp_opt = (
      n->cond->kind == EXPR_ID &&
      type_isopt(n->cond->type) &&
      !type_isptrlike(((const opttype_t*)n->cond->type)->elem)
    );
    const idexpr_t* id = NULL;

    if (has_tmp_opt) {
      // e.g. "let x ?int; let y = if x { use(x) }"
      id = (const idexpr_t*)n->cond;
      if (node_isexpr(id->ref) && ((const expr_t*)id->ref)->nrefs > 1) {
        g->indent++;
        if (n->flags & NF_RVALUE)
          CHAR('(');
        PRINT("{ ");
        opttype(g, (const opttype_t*)n->cond->type);
        fmt_tmp_id(tmp, sizeof(tmp), id);
        PRINTF(" %s = %s; ", tmp, id->name);

        // "T x = tmp.v;"
        type(g, ((const opttype_t*)n->cond->type)->elem);
        PRINTF(" %s = %s.v; ", id->name, tmp);
      } else {
        has_tmp_opt = false;
      }
    }

    if (n->flags & NF_RVALUE) {
      if (id) {
        PRINTF("(%s.ok ? ", has_tmp_opt ? tmp : id->name);
      } else {
        CHAR('('), expr_as_value(g, n->cond);
        if (n->cond->type->kind == TYPE_OPTIONAL &&
          !type_isptrlike(((const opttype_t*)n->cond->type)->elem))
        {
          PRINT(".ok");
        }
        PRINT(" ? ");
      }
    } else {
      if (id) {
        PRINTF("if (%s.ok) ", has_tmp_opt ? tmp : id->name);
      } else {
        PRINT("if ("), expr_as_value(g, n->cond), CHAR(')');
      }
    }
  }

  if (n->flags & NF_RVALUE) {
    startline(g, n->thenb->loc);

    if ((!n->elseb || n->elseb->type == type_void) && !type_isopt(n->thenb->type)) {
      optinit(g, (expr_t*)n->thenb, /*isshort*/false);
    } else {
      block(g, n->thenb, 0); // expr_as_value(g, n->thenb);
    }
    PRINT(" : (");
    if (n->elseb) {
      if (loc_line(n->elseb->loc) != g->lineno)
        startline(g, n->elseb->loc);
      block(g, n->elseb, 0);
      if (n->elseb->type == type_void)
        PRINT(", ");
    } else {
      CHAR(' ');
    }
    if (!n->elseb || n->elseb->type == type_void) {
      type_t* elem = n->thenb->type;
      if (type_isopt(elem))
        elem = ((const opttype_t*)elem)->elem;
      optzero(g, elem, /*isshort*/false);
    }
    PRINT("))");
  } else {
    block(g, n->thenb, 0);
    if (n->elseb) {
      if (lastchar(g) != '}')
        CHAR(';'); // terminate non-block "then" body
      PRINT(" else ");
      block(g, n->elseb, 0);
    }
  }

  if (n->flags & NF_RVALUE)
    g->indent--;

  if (hasvar || has_tmp_opt) {
    g->indent--;
    bool needsemi = lastchar(g) != '}';
    if (needsemi)
      CHAR(';');
    startlinex(g);
    CHAR('}');
    if (n->flags & NF_RVALUE)
      CHAR(')');
  }
}


static void forexpr(cgen_t* g, const forexpr_t* n) {
  PRINT("for (");
  if (n->start)
    expr(g, n->start);
  PRINT("; ");
  expr(g, n->cond);
  PRINT("; ");
  if (n->end)
    expr(g, n->end);
  PRINT(") ");
  expr_in_block(g, n->body);
}


static void typedef_(cgen_t* g, const typedef_t* n) {
  intern_typedef(g, (type_t*)&n->type, gen_alias_typename, gen_alias_typedef);
}


static void expr(cgen_t* g, const expr_t* n) {
  switch ((enum nodekind)n->kind) {
  case EXPR_FUN:       return fun(g, (const fun_t*)n);
  case EXPR_INTLIT:    return intlit(g, (const intlit_t*)n);
  case EXPR_FLOATLIT:  return floatlit(g, (const floatlit_t*)n);
  case EXPR_BOOLLIT:   return boollit(g, (const intlit_t*)n);
  case EXPR_ID:        return idexpr(g, (const idexpr_t*)n);
  case EXPR_PARAM:     return param(g, (const local_t*)n);
  case EXPR_BLOCK:     return block(g, (const block_t*)n, 0);
  case EXPR_CALL:      return call(g, (const call_t*)n);
  case EXPR_MEMBER:    return member(g, (const member_t*)n);
  case EXPR_IF:        return ifexpr(g, (const ifexpr_t*)n);
  case EXPR_FOR:       return forexpr(g, (const forexpr_t*)n);
  case EXPR_RETURN:    return retexpr(g, (const retexpr_t*)n, NULL);
  case EXPR_DEREF:
  case EXPR_PREFIXOP:  return prefixop(g, (const unaryop_t*)n);
  case EXPR_POSTFIXOP: return postfixop(g, (const unaryop_t*)n);
  case EXPR_BINOP:     return binop(g, (const binop_t*)n);
  case EXPR_ASSIGN:    return assign(g, (const binop_t*)n);

  case EXPR_VAR:
  case EXPR_LET:
    return vardef(g, (const local_t*)n);

  // node types we should never see
  case NODEKIND_COUNT:
  case NODE_BAD:
  case NODE_COMMENT:
  case NODE_UNIT:
  case STMT_TYPEDEF:
  case EXPR_FIELD:
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
  case TYPE_FUN:
  case TYPE_PTR:
  case TYPE_REF:
  case TYPE_OPTIONAL:
  case TYPE_STRUCT:
  case TYPE_ALIAS:
  case TYPE_UNKNOWN:
  case TYPE_UNRESOLVED:
    break;
  }
  debugdie(g, n, "unexpected node %s", nodekind_name(n->kind));
}


static void unit(cgen_t* g, const unit_t* n) {
  if (n->children.len == 0)
    return;

  u32 init_inputid = g->inputid;
  u32 init_lineno = g->lineno;

  // typedefs
  for (u32 i = 0; i < n->children.len; i++) {
    const typedef_t* td = n->children.v[i];
    if (td->kind == STMT_TYPEDEF) {
      sizetuple_t startlens = x_semi_begin_startline(g, td->loc);
      typedef_(g, td);
      x_semi_end(g, startlens);
    }
  }

  // function prototypes
  g->inputid = init_inputid;
  g->lineno = init_lineno;
  for (u32 i = 0; i < n->children.len; i++) {
    const fun_t* fn = n->children.v[i];
    if (fn->kind == EXPR_FUN) {
      startline(g, fn->loc);
      fun_proto(g, fn);
      CHAR(';');
    }
  }

  // function implementations
  g->inputid = init_inputid;
  g->lineno = init_lineno;
  for (u32 i = 0; i < n->children.len; i++) {
    const fun_t* fn = n->children.v[i];
    if (fn->kind == EXPR_FUN) {
      if (fn->body == NULL) {
        // declaration-only function already generated
        continue;
      }
      startline(g, fn->loc);
      fun(g, fn);
    } else if (fn->kind != STMT_TYPEDEF) {
      debugdie(g, n, "unexpected unit-level node %s", nodekind_name(n->kind));
    }
  }
}


err_t cgen_generate(cgen_t* g, const unit_t* n) {
  // reset generator state
  g->err = 0;
  buf_clear(&g->outbuf);
  buf_clear(&g->headbuf);
  map_clear(&g->typedefmap);
  map_clear(&g->tmpmap);
  g->anon_idgen = 0;
  g->inputid = 0;
  g->lineno = 0;
  g->scopenest = 0;
  g->headnest = 0;

  PRINT("#include <c0prelude.h>\n");

  input_t* input = loc_input(n->loc, locmap(g));
  if (input) {
    g->inputid = loc_inputid(n->loc);
    g->lineno = 1;
    PRINTF("\n#line 1 \"%s\"\n", input->name);
  }

  usize headstart = g->outbuf.len;

  if (n->kind != NODE_UNIT)
    return ErrInvalid;
  unit(g, n);

  if (g->headbuf.len > 0) {
    if (!buf_insert(&g->outbuf, headstart, g->headbuf.p, g->headbuf.len))
      seterr(g, ErrNoMem);
  }

  // make sure outputs ends with LF
  if (g->outbuf.len > 0 && g->outbuf.chars[g->outbuf.len-1] != '\n')
    CHAR('\n');

  // check if we ran out of memory by appending \0 without affecting len
  if (!buf_nullterm(&g->outbuf))
    seterr(g, ErrNoMem);

  return g->err;
}
