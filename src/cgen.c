// SPDX-License-Identifier: Apache-2.0
#include "colib.h"
#include "compiler.h"
#include "path.h"
#include <sys/stat.h>


//#define INTERNAL_SEP  "·" // U+00B7 MIDDLE DOT (UTF8: "\xC2\xB7")
#define ANON_PREFIX    CO_INTERNAL_PREFIX "v"
#define ANON_FMT       ANON_PREFIX "%x"


typedef struct { usize v[2]; } sizetuple_t;


#define trace(fmt, va...) \
  _trace(opt_trace_cgen, 6, "cgen", "%*s" fmt, g->traceindent, "", ##va)

#ifdef DEBUG
  #define trace_indentinc() (g->traceindent++)
  #define trace_indentdec() (g->traceindent--)
#else
  #define trace_indentinc() ((void)0)
  #define trace_indentdec() ((void)0)
#endif


// REPRNODE is used for trace and dlog
#define REPRNODE_FMT "%s#%p %s"
#define REPRNODE_ARGS(n, bufno) \
  nodekind_name((n)->kind), (n), fmtnode(g,(bufno),(n))


bool cgen_init(
  cgen_t* g, compiler_t* c, const pkg_t* pkg, memalloc_t out_ma, u32 flags)
{
  memset(g, 0, sizeof(*g));
  g->compiler = c;
  g->pkg = pkg;
  g->ma = c->ma;
  g->flags = flags;
  buf_init(&g->outbuf, out_ma);
  buf_init(&g->headbuf, out_ma);

  if (!map_init(&g->typedefmap, g->ma, 32))
    return false;
  if (!map_init(&g->tmpmap, g->ma, 32)) {
    map_dispose(&g->typedefmap, g->ma);
    return false;
  }
  return true;
}


void cgen_dispose(cgen_t* g) {
  if (g->ma == NULL)
    return;
  map_dispose(&g->tmpmap, g->ma);
  map_dispose(&g->typedefmap, g->ma);
  buf_dispose(&g->outbuf);
  buf_dispose(&g->headbuf);
  ptrarray_dispose(&g->tmpptrarray, g->ma);
}


static void cgen_reset(cgen_t* g) {
  buf_clear(&g->outbuf);
  buf_clear(&g->headbuf);
  g->headoffs = 0;
  g->headnest = 0;
  g->headlineno = 0;
  g->headsrcfileid = 0;
  g->srcfileid = 0;
  g->lineno = 0;
  g->scopenest = 0;
  g->err = 0;
  g->idgen_local = 0;
  g->indent = 0;
  map_clear(&g->typedefmap);
  map_clear(&g->tmpmap);
  g->mainfun = NULL;
}


inline static locmap_t* locmap(cgen_t* g) {
  return &g->compiler->locmap;
}


static void seterr(cgen_t* g, err_t err) {
  #if DEBUG
    dlog("cgen seterr \"%s\"", err_str(err));
    //fprint_stacktrace(stderr, /*frame_offset*/1);
  #endif
  if (!g->err)
    g->err = err;
}


#define error(g, node_or_type, fmt, args...) \
  _error((g), ast_origin(locmap(g), (node_t*)node_or_type), fmt, ##args)


ATTR_FORMAT(printf,3,4)
static void _error(cgen_t* g, origin_t origin, const char* fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  report_diagv(g->compiler, origin, DIAG_ERR, fmt, ap);
  va_end(ap);
  seterr(g, ErrInvalid);
}


UNUSED static const char* fmtnode(cgen_t* g, u32 bufindex, const void* nullable n) {
  buf_t* buf = tmpbuf_get(bufindex);
  err_t err = node_fmt(buf, n, /*depth*/0);
  if (!err)
    return buf->chars;
  dlog("node_fmt: %s", err_str(err));
  seterr(g, err);
  return "?";
}


#if DEBUG
  #define debugdie(g, node_or_type, fmt, args...) ( \
    error((g), (node_or_type), fmt, ##args), \
    panic("code generator got unexpected AST") \
  )
#else
  #define debugdie(...) ((void)0)
#endif


#define CHAR(ch)             buf_push(&g->outbuf, (ch))
#define PRINT(cstr)          buf_print(&g->outbuf, (cstr))
#define PRINTF(fmt, args...) buf_printf(&g->outbuf, (fmt), ##args)
#define PRINTN(bytes, len)   buf_append(&g->outbuf, (bytes), (len))


static char lastchar(cgen_t* g) {
  assert(g->outbuf.len > 0);
  return g->outbuf.chars[g->outbuf.len-1];
}


static bool startloc(cgen_t* g, loc_t loc) {
  bool inputok = loc_srcfileid(loc) == 0 || g->srcfileid == loc_srcfileid(loc);
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
      const srcfile_t* sf = assertnotnull(loc_srcfile(loc, locmap(g)));
      g->srcfileid = loc_srcfileid(loc);
      // ` "pkgdir/file.co"`
      PRINT(" \"");
      buf_appendrepr(&g->outbuf, sf->pkg->dir.p, sf->pkg->dir.len);
      CHAR(PATH_SEP);
      buf_appendrepr(&g->outbuf, sf->name.p, sf->name.len);
      CHAR('"');
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


static char* mangle(cgen_t* g, const node_t* n) {
  buf_t* buf = tmpbuf_get(0);
  if (compiler_mangle(g->compiler, g->pkg, buf, n)) {
    memalloc_t ast_ma = g->ma; // FIXME pass acutual ast_ma to cgen
    char* s = mem_strdup(ast_ma, buf_slice(*buf), 0);
    if (s)
      return s;
  }
  seterr(g, ErrNoMem);
  static char last_resort[1] = {0};
  return last_resort;
}


static void gen_type(cgen_t* g, const type_t*);
static void gen_expr(cgen_t* g, const expr_t*);
static void gen_expr_rvalue(cgen_t* g, const expr_t* n, const type_t* dst_type);
static void gen_intconst(cgen_t* g, u64 value, const type_t* t);


static const char* operator(op_t op) {
  switch ((enum op)op) {
  case OP_ALIAS:
  case OP_ARG:
  case OP_REF:
  case OP_MUTREF:
  case OP_CALL:
  case OP_DROP:
  case OP_FCONST:
  case OP_FUN:
  case OP_ICONST:
  case OP_ARRAY:
  case OP_STR:
  case OP_MOVE:
  case OP_NOOP:
  case OP_OCHECK:
  case OP_PHI:
  case OP_STORE:
  case OP_VAR:
  case OP_ZERO:
  case OP_CAST:
  case OP_GEP:
    // bad op
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


static const type_t* unwind_aliastypes(const type_t* t) {
  while (t->kind == TYPE_ALIAS)
    t = assertnotnull(((aliastype_t*)t)->elem);
  return t;
}


static void gen_funtype(cgen_t* g, const funtype_t* t, const char* nullable name) {
  // void(*name)(args)
  gen_type(g, t->result);
  PRINT("(*");
  if (!name || name == sym__) {
    PRINTF(ANON_FMT, g->idgen_local++);
  } else {
    PRINT(name);
  }
  PRINT(")(");
  if (t->params.len == 0) {
    PRINT("void");
  } else {
    for (u32 i = 0; i < t->params.len; i++) {
      local_t* param = (local_t*)t->params.v[i];
      assert(param->kind == EXPR_PARAM);
      // if (!type_isprim(param->type) && !param->ismut)
      //   PRINT("const ");
      if (i) PRINT(", ");
      gen_type(g, param->type);
      if (param->name && param->name != sym__) {
        CHAR(' ');
        PRINT(param->name);
      }
    }
  }
  CHAR(')');
}



static void gen_structtype(cgen_t* g, const structtype_t* st) {
  if (st->mangledname) {
    PRINT("struct "), PRINT(st->mangledname);
  } else {
    PRINTF("struct " CO_TYPE_PREFIX "struct%lx" CO_TYPE_SUFFIX, (uintptr)st);
  }
}


static void gen_structtype_def(cgen_t* g, structtype_t* st) {
  startline(g, st->loc);
  gen_structtype(g, st), PRINT(" {");
  if (st->fields.len == 0) {
    PRINT("u8 _unused;");
  } else {
    g->indent++;
    u32 start_lineno = g->lineno;
    const type_t* t = NULL;
    for (u32 i = 0; i < st->fields.len; i++) {
      const local_t* f = (local_t*)st->fields.v[i];
      bool newline = loc_line(f->loc) != g->lineno;
      if (newline) {
        if (i) CHAR(';');
        t = NULL;
        startline(g, f->loc);
      }
      if (f->type != t) {
        if (i && !newline) PRINT("; ");
        if (f->type->kind == TYPE_FUN) {
          gen_funtype(g, (const funtype_t*)f->type, f->name);
          continue;
        }
        gen_type(g, f->type);
        CHAR(' ');
        t = f->type;
      } else {
        PRINT(", ");
      }
      PRINT(f->name);
    }
    CHAR(';');
    g->indent--;
    if (g->lineno != start_lineno)
      startlinex(g);
  }
  PRINT("};");
}


// static void maybe_gen_ptrtype_defguard(cgen_t* g, const ptrtype_t* t) {
//   if (nodekind_isprimtype(t->elem->kind)) {
//     assertnotnull(t->mangledname);
//     PRINTF("\n#ifndef __co_DEF_%s", t->mangledname), g->lineno++;
//     PRINTF("\n#define __co_DEF_%s", t->mangledname), g->lineno++;
//   }
// }

// static void maybe_gen_ptrtype_defguard_end(cgen_t* g, const ptrtype_t* t) {
//   if (nodekind_isprimtype(t->elem->kind))
//     PRINT("\n#endif"), g->lineno++;
// }


static void gen_slicetype(cgen_t* g, const slicetype_t* t) {
  PRINT("struct "), PRINT(assertnotnull(t->mangledname));
}

static void gen_slicetype_def(cgen_t* g, const slicetype_t* t) {
  if (nodekind_isprimtype(t->elem->kind)) {
    // predefined in prelude
    return;
  }
  //maybe_gen_ptrtype_defguard(g, (ptrtype_t*)t);
  startline(g, t->loc);
  gen_slicetype(g, t), PRINT(" {");
  gen_type(g, g->compiler->uinttype);
  PRINT(" len; ");
  if (t->kind == TYPE_SLICE) // not "mut"
    PRINT("const ");
  gen_type(g, t->elem);
  PRINT("* ptr;};");
  //maybe_gen_ptrtype_defguard_end(g, (ptrtype_t*)t);
}


static void gen_arraytype(cgen_t* g, const arraytype_t* t) {
  if (t->len > 0) {
    gen_type(g, t->elem), CHAR('*');
  } else {
    PRINT("struct "), PRINT(assertnotnull(t->mangledname));
  }
}

static void gen_arraytype_def(cgen_t* g, const arraytype_t* t) {
  if (t->len > 0) {
    // statically-sized array (T*) needs no definition
    return;
  }
  // dynamic array -- typedef struct { uint cap, len; T* ptr; }
  if (nodekind_isprimtype(t->elem->kind)) {
    // predefined in prelude
    return;
  }
  //maybe_gen_ptrtype_defguard(g, (ptrtype_t*)t);
  startline(g, t->loc);
  gen_arraytype(g, t), PRINT(" {");
  gen_type(g, g->compiler->uinttype);
  PRINT(" cap, len; ");
  gen_type(g, t->elem);
  PRINT("* ptr;};");
  //maybe_gen_ptrtype_defguard_end(g, (ptrtype_t*)t);
}


// reftype_byvalue returns true if &T is passed by value rather than by reference
static bool reftype_byvalue(cgen_t* g, const reftype_t* t) {
  return ( t->elem->kind == TYPE_SLICE ||
           t->elem->kind == TYPE_MUTSLICE ||
           t->elem->kind == TYPE_ARRAY ||
           ( t->kind == TYPE_REF &&
             t->elem->size <= (u64)g->compiler->target.ptrsize*2 )
  );
}


static void gen_reftype(cgen_t* g, const reftype_t* t) {
  if (reftype_byvalue(g, t)) {
    // e.g. "&Foo" => "Foo"
    if (t->kind == TYPE_REF && t->elem->kind == TYPE_ARRAY &&
        ((arraytype_t*)t->elem)->len != 0)
    {
      // note: we can't use "const" here for plain types since that would cause
      // issues with temorary locals, for example implicit block returns as in
      // "let y = { ...; x }" which uses a temporary variable to store x in for
      // the block "{ ...; x }".
      // However for pointer values like arrays we _must_ use const.
      PRINT("const ");
    }
    gen_type(g, t->elem);
  } else {
    // e.g. "&Foo"    => "const Foo*"
    //      "mut&Foo" => "Foo*"
    if (t->kind == TYPE_REF)
      PRINT("const ");
    gen_type(g, t->elem);
    CHAR('*');
  }
}


static void gen_ptrtype(cgen_t* g, const ptrtype_t* t) {
  gen_type(g, t->elem);
  CHAR('*');
}


static void gen_aliastype(cgen_t* g, const aliastype_t* t) {
  PRINT(assertnotnull(t->mangledname));
}

static void gen_aliastype_def(cgen_t* g, const aliastype_t* t) {
  if (t == &g->compiler->strtype)
    return;
  startline(g, t->loc);
  PRINT("typedef "), gen_type(g, t->elem);
  PRINTF(" %s;", assertnotnull(t->mangledname));
}


static void gen_opttype_name(cgen_t* g, const opttype_t* t) {
  assertnotnull(t->mangledname);
  PRINT("struct "), PRINT(assertnotnull(t->mangledname));
  // PRINTF(CO_TYPE_PREFIX "opt%lx" CO_TYPE_SUFFIX, (uintptr)t);
}

static void gen_opttype_def(cgen_t* g, opttype_t* t) {
  // ?*T is represented as a nullable pointer (not a struct) e.g. "T*"
  if (type_isptrlike(t->elem))
    return;
  startline(g, t->loc);
  gen_opttype_name(g, t);
  PRINT("{bool ok; "); gen_type(g, t->elem); PRINT(" v;};");
}

static void gen_opttype(cgen_t* g, const opttype_t* t) {
  // ?*T is represented as a nullable pointer (not a struct) e.g. "T*"
  if (type_isptrlike(t->elem))
    return gen_type(g, t->elem);
  gen_opttype_name(g, t);
}


static void gen_optinit(cgen_t* g, const expr_t* init, bool isshort) {
  if (type_isptrlike(init->type))
    return gen_expr_rvalue(g, init, init->type);
  assert(init->type->kind != TYPE_OPTIONAL);
  if (!isshort) {
    opttype_t t = { .kind = TYPE_OPTIONAL, .elem = (type_t*)init->type };
    CHAR('('), gen_opttype(g, &t), CHAR(')');
  }
  PRINT("{true,"); gen_expr_rvalue(g, init, init->type); CHAR('}');
}


static void gen_optzero(cgen_t* g, const type_t* elem, bool isshort) {
  assert(elem->kind != TYPE_OPTIONAL);
  if (type_isptrlike(elem)) {
    PRINT("NULL");
  } else {
    if (!isshort) {
      opttype_t t = { .kind = TYPE_OPTIONAL, .elem = (type_t*)elem };
      CHAR('('), gen_opttype(g, &t), CHAR(')');
    }
    PRINT("{0}");
  }
}


static void gen_type(cgen_t* g, const type_t* t) {
  switch (t->kind) {
  case TYPE_VOID:
  case TYPE_BOOL:
  case TYPE_I8:
  case TYPE_I16:
  case TYPE_I32:
  case TYPE_I64:
  case TYPE_U8:
  case TYPE_U16:
  case TYPE_U32:
  case TYPE_U64:
  case TYPE_F32:
  case TYPE_F64:
    PRINT(primtype_name(t->kind));
    break;
  case TYPE_INT:      return gen_type(g, g->compiler->inttype);
  case TYPE_UINT:     return gen_type(g, g->compiler->uinttype);
  // case TYPE_INT:      PRINT(CO_TYPE_PREFIX "int"); break;
  // case TYPE_UINT:     PRINT(CO_TYPE_PREFIX "uint"); break;

  case TYPE_FUN:      return gen_funtype(g, (const funtype_t*)t, NULL);
  case TYPE_PTR:      return gen_ptrtype(g, (const ptrtype_t*)t);
  case TYPE_REF:
  case TYPE_MUTREF:   return gen_reftype(g, (const reftype_t*)t);
  case TYPE_SLICE:
  case TYPE_MUTSLICE: return gen_slicetype(g, (const slicetype_t*)t);
  case TYPE_OPTIONAL: return gen_opttype(g, (const opttype_t*)t);
  case TYPE_STRUCT:   return gen_structtype(g, (const structtype_t*)t);
  case TYPE_ALIAS:    return gen_aliastype(g, (const aliastype_t*)t);
  case TYPE_ARRAY:    return gen_arraytype(g, (const arraytype_t*)t);

  default:
    panic("unexpected type_t %s (%u)", nodekind_name(t->kind), t->kind);
  }
}


// true if n needs to be wrapped in "(...)" for op C <> Compis precedence differences
static bool has_ambiguous_prec(const expr_t* n) {
  switch (n->kind) {
  case EXPR_INTLIT:
  case EXPR_FLOATLIT:
  case EXPR_ID:
  case EXPR_PARAM:
  case EXPR_MEMBER:
  case EXPR_BLOCK:
  case EXPR_CALL:
  case EXPR_DEREF:
  case EXPR_PREFIXOP:
  case EXPR_POSTFIXOP:
    return false;
  default:
    return true;
  }
}


static void gen_expr_rvalue(cgen_t* g, const expr_t* n, const type_t* lt) {
  const type_t* rt = n->type;
  bool parenwrap = has_ambiguous_prec(n);
  if (parenwrap)
    CHAR('(');

  if (lt->kind != rt->kind) {
    lt = unwind_aliastypes(lt);
    rt = unwind_aliastypes(rt);
    if (lt->kind != rt->kind) switch (lt->kind) {
      case TYPE_OPTIONAL:
        // ?T <= T
        return gen_optinit(g, n, /*isshort*/false);

      case TYPE_SLICE:
      case TYPE_MUTSLICE:
        // &[T] <= &[T N]
        assertf(type_isref(rt) && ((reftype_t*)rt)->elem->kind == TYPE_ARRAY,
          "unexpected slice initializer %s", nodekind_name(rt->kind) );
        const arraytype_t* at = (arraytype_t*)((reftype_t*)rt)->elem;
        // struct slice { uint len; T* ptr; }
        CHAR('('), gen_type(g, (type_t*)lt), PRINT("){");
        gen_intconst(g, at->len, g->compiler->uinttype);
        CHAR(',');
        gen_expr(g, n);
        CHAR('}');
        if (parenwrap)
          CHAR(')');
        return;

      case TYPE_REF: {
        if (rt->kind == TYPE_MUTREF && reftype_byvalue(g, (reftype_t*)lt)) {
          // special case: "&T <= mut&T" where &T is byvalue requires deref,
          // since "mut&T" is always by pointer.
          CHAR('*');
        }
        break;
      }
    }
  }

  if (parenwrap) {
    gen_expr(g, n);
    CHAR(')');
  } else {
    // allow tail call in common case
    return gen_expr(g, n);
  }
}


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


static void gen_zeroinit(cgen_t* g, const type_t* t);


static void expr_or_zeroinit(cgen_t* g, const type_t* t, const expr_t* nullable n) {
  if (n)
    return gen_expr(g, n);
  gen_zeroinit(g, t);
}


static void gen_retexpr(cgen_t* g, const retexpr_t* n, const char* nullable tmp) {
  if (tmp) {
    assert(n->type != type_void);
    gen_type(g, n->type), PRINT(" const "), PRINT(tmp);
    if (!n->value)
      return;
    PRINT(" = ");
    if (!n->value) {
      gen_zeroinit(g, n->type);
    } else {
      gen_expr(g, n->value);
    }
  } else if (!n->value) {
    PRINT("return");
  } else {
    PRINT("return "), gen_expr(g, n->value);
  }
}


static void as_ptr(cgen_t* g, buf_t* buf, const type_t* t, const char* name) {
  switch (t->kind) {
    case TYPE_PTR:
      buf_print(buf, name);
      break;
    case TYPE_OPTIONAL:
      if (((const opttype_t*)t)->elem->kind == TYPE_PTR) {
        buf_print(buf, name);
      } else {
        buf_push(buf, '&');
        buf_print(buf, name);
        buf_print(buf, ".v");
      }
      break;
    default:
      buf_push(buf, '&');
      buf_print(buf, name);
  }
}


// unwrap_ptr_and_alias unwraps optional, ref, ptr and alias
// e.g. "?&MyT" => "&MyT" => "MyT" => "T"
static type_t* unwrap_ptr_and_alias(type_t* t) {
  assertnotnull(t);
  for (;;) switch (t->kind) {
    case TYPE_OPTIONAL: t = assertnotnull(((opttype_t*)t)->elem); break;
    case TYPE_REF:
    case TYPE_MUTREF:   t = assertnotnull(((reftype_t*)t)->elem); break;
    case TYPE_PTR:      t = assertnotnull(((ptrtype_t*)t)->elem); break;
    case TYPE_ALIAS:    t = assertnotnull(((aliastype_t*)t)->elem); break;
    default:            return t;
  }
}


static void drop_begin(cgen_t* g, const expr_t* owner) {
  // TODO FIXME
  dlog("TODO FIXME %s", __FUNCTION__);
  PRINT(type_isopt(owner->type) ?
    CO_INTERNAL_PREFIX "drop_opt(" :
    CO_INTERNAL_PREFIX "drop(");
}


static void drop_end(cgen_t* g) {
  CHAR(')');
}


static void error_ownership_cycle_help(
  cgen_t* g, const type_t* bt, const node_t* origin_n)
{
  origin_t origin = ast_origin(locmap(g), origin_n);
  switch (origin_n->kind) {

  case EXPR_FIELD:
    report_diag(g->compiler, origin, DIAG_HELP,
      "field \"%s\" of managed-lifetime type %s",
      ((local_t*)origin_n)->name, fmtnode(g, 0, bt));
    break;

  case TYPE_ALIAS:
    report_diag(g->compiler, origin, DIAG_HELP,
      "type alias \"%s\" of managed-lifetime type %s",
      ((aliastype_t*)origin_n)->name, fmtnode(g, 0, bt));
    break;

  case TYPE_ARRAY:
    report_diag(g->compiler, origin, DIAG_HELP,
      "array of managed-lifetime type %s", fmtnode(g, 0, bt));
    break;

  default:
    report_diag(g->compiler, origin, DIAG_HELP,
      "%s %s of managed-lifetime type %s",
      nodekind_fmt(origin_n->kind), fmtnode(g, 1, origin_n), fmtnode(g, 0, bt));
  }
}


static u32 drop_visitstack_indexof(ptrarray_t* visitstack, const type_t* bt) {
  for (u32 i = 0; i < visitstack->len; i += 2) {
    if (visitstack->v[i] == bt)
      return i;
  }
  return U32_MAX;
}


static void error_ownership_cycle(
  cgen_t* g, ptrarray_t* visitstack, const type_t* bt, const node_t* origin_n)
{
  // generate helpful "path"
  buf_t buf = buf_make(g->ma);
  buf_print(&buf, " (");
  u32 index = drop_visitstack_indexof(visitstack, bt);
  assertf(index % 2 == 0, "even index");
  assertf(visitstack->len % 2 == 0, "even number of entries");
  for (u32 i = index; i < visitstack->len; i += 2) {
    node_fmt(&buf, visitstack->v[i], /*depth*/0);
    buf_print(&buf, " -> ");
  }
  node_fmt(&buf, (node_t*)bt, /*depth*/0);
  buf_print(&buf, ")");
  if (buf.oom)
    buf.len = 0;

  // emit diagnostic (error)
  error(g, origin_n, "ownership cycle: %s manages its own lifetime%.*s",
    fmtnode(g, 0, bt), (int)buf.len, buf.chars);

  buf_dispose(&buf);

  // emit diagnostic (help)
  // error_ownership_cycle_help(g, bt, origin_n);
  // for (u32 i = visitstack->len; i > index;) {
  //   origin_n = visitstack->v[--i];
  //   bt = visitstack->v[--i];
  //   error_ownership_cycle_help(g, bt, origin_n);
  // }
  for (u32 i = index; i < visitstack->len;) {
    const type_t* bt = visitstack->v[i++];
    const node_t* origin_n = visitstack->v[i++];
    error_ownership_cycle_help(g, bt, origin_n);
  }
  error_ownership_cycle_help(g, bt, origin_n);
}


static bool drop_visitstack_push(
  cgen_t* g, ptrarray_t* visitstack, const type_t* bt, const node_t* origin_n)
{
  // detect cycles
  if UNLIKELY(drop_visitstack_indexof(visitstack, bt) < U32_MAX) {
    error_ownership_cycle(g, visitstack, bt, origin_n);
    return false;
  }
  void** p = ptrarray_alloc(visitstack, g->ma, 2);
  if UNLIKELY(!p) {
    seterr(g, ErrNoMem);
    return false;
  }
  p[0] = (node_t*)bt;
  p[1] = (node_t*)origin_n;
  assert(visitstack->len > 1);
  assert(visitstack->len % 2 == 0);
  return true;
}


static void drop_visitstack_pop(ptrarray_t* visitstack) {
  assert(visitstack->len > 1);
  visitstack->len -= 2;
}


static void gen_drop(cgen_t* g, ptrarray_t* visitstack, const drop_t* d);


static void gen_drop_custom(cgen_t* g, const drop_t* d, const type_t* bt) {
  const char* mangledname = "?";
  switch (bt->kind) {
    case TYPE_STRUCT:
      mangledname = ((structtype_t*)bt)->mangledname;
      break;
    default:
      assertf(0, "unexpected %s", nodekind_name(bt->kind));
  }

  PRINTF("Nf%s4drop(", mangledname);
  as_ptr(g, &g->outbuf, d->type, d->name);
  PRINT(");");
}


static void gen_drop_struct_fields(
  cgen_t* g, ptrarray_t* visitstack, const drop_t* d, const structtype_t* st)
{
  // dlog(" gen_drop_struct_fields");
  buf_t tmpbuf = buf_make(g->ma);
  for (u32 i = st->fields.len; i; ) {
    const local_t* field = (local_t*)st->fields.v[--i];
    const type_t* ft = field->type;

    if (!type_isowner(ft)) {
      // dlog("  field (" REPRNODE_FMT ") of type (" REPRNODE_FMT ") is not owner",
      //   REPRNODE_ARGS(field, 0), REPRNODE_ARGS(ft, 1));
      continue;
    }

    const type_t* bt = type_unwrap_ptr((type_t*)ft);
    if (!drop_visitstack_push(g, visitstack, bt, (node_t*)field))
      break;

    // dlog("drop field %s", field->name);

    buf_clear(&tmpbuf);

    buf_push(&tmpbuf, '(');
    as_ptr(g, &tmpbuf, d->type, d->name);
    buf_printf(&tmpbuf, ")->%s", field->name);

    if UNLIKELY(!buf_nullterm(&tmpbuf))
      return seterr(g, ErrNoMem);

    drop_t d2 = { .name = tmpbuf.chars, .type = field->type };
    gen_drop(g, visitstack, &d2);

    drop_visitstack_pop(visitstack);
  }
  buf_dispose(&tmpbuf);
}


static void gen_drop_array_elements(
  cgen_t* g, ptrarray_t* visitstack, const drop_t* d, const arraytype_t* at)
{
  if (!drop_visitstack_push(g, visitstack, (type_t*)at->elem, (node_t*)d->type))
    return;

  char key_id[strlen(ANON_PREFIX "FFFFFFFF") + 1];
  snprintf(key_id, sizeof(key_id), ANON_PREFIX "%x", g->idgen_local++);

  buf_t tmpbuf = buf_make(g->ma);
  startlinex(g);

  if (at->len == 0) {
    // dynamic runtime-sized array
    PRINTF("for (__co_uint %s = 0; %s < %s.len; %s++) {",
      key_id, key_id, d->name, key_id);
    buf_printf(&tmpbuf, "%s.ptr[%s]", d->name, key_id);
  } else {
    // compile time-sized array
    PRINTF("for (__co_uint %s = 0; %s < %llu; %s++) {",
      key_id, key_id, at->len, key_id);
    buf_printf(&tmpbuf, "%s[%s]", d->name, key_id);
  }

  g->indent++;
  drop_t d2 = { .name = tmpbuf.chars, .type = at->elem };
  gen_drop(g, visitstack, &d2);
  g->indent--;

  buf_dispose(&tmpbuf);

  startlinex(g);
  PRINT("}");

  drop_visitstack_pop(visitstack);
}


static void gen_drop_alias_elem(
  cgen_t* g, ptrarray_t* visitstack, const drop_t* d, const aliastype_t* at)
{
  if (!drop_visitstack_push(g, visitstack, (type_t*)at->elem, (node_t*)d->type))
    return;
  drop_t d2 = { .name = d->name, .type = at->elem };
  gen_drop(g, visitstack, &d2);
  drop_visitstack_pop(visitstack);
}


static void gen_drop_subowners(
  cgen_t* g, ptrarray_t* visitstack, const drop_t* d, const type_t* bt)
{
  switch (bt->kind) {
    case TYPE_STRUCT: return gen_drop_struct_fields(g, visitstack, d, (structtype_t*)bt);
    case TYPE_ARRAY:  return gen_drop_array_elements(g, visitstack, d, (arraytype_t*)bt);
    case TYPE_ALIAS:  return gen_drop_alias_elem(g, visitstack, d, (aliastype_t*)bt);
    default:
      if (!type_isprim(bt))
        assertf(0, "NOT IMPLEMENTED %s", nodekind_name(bt->kind));
  }
}


static void gen_drop_ptr(cgen_t* g, const drop_t* d, const ptrtype_t* pt) {
  startlinex(g);
  PRINTF(CO_INTERNAL_PREFIX "mem_free(%s, %llu);", d->name, pt->elem->size);
}


static void gen_drop_array(cgen_t* g, const drop_t* d, const arraytype_t* at) {
  startlinex(g);
  if (at->len == 0) {
    // dynamic runtime-sized array
    PRINTF(CO_INTERNAL_PREFIX "mem_free(%s.ptr, %s.cap * %llu);",
      d->name, d->name, at->elem->size);
  } else {
    // compile time-sized array
    PRINTF(CO_INTERNAL_PREFIX "mem_free(%s, %llu);",
      d->name, at->len * at->elem->size);
  }
}


static void gen_drop(cgen_t* g, ptrarray_t* visitstack, const drop_t* d) {
  const type_t* effective_type = d->type;
  const type_t* bt = type_unwrap_ptr((type_t*)effective_type);

  // dlog("drop \"%s\" " REPRNODE_FMT, d->name, REPRNODE_ARGS(d->type, 0));

  startlinex(g);
  if (effective_type->kind == TYPE_OPTIONAL) {
    effective_type = ((opttype_t*)effective_type)->elem;
    if (type_isptr(effective_type)) {
      PRINTF("if (%s) {", d->name);
    } else {
      PRINTF("if (%s.ok) {", d->name);
    }
    g->indent++;
    startlinex(g);
  }

  if (bt->flags & NF_DROP)
    gen_drop_custom(g, d, bt);

  if (bt->flags & NF_SUBOWNERS)
    gen_drop_subowners(g, visitstack, d, bt);

  if (type_isptr(effective_type)) {
    gen_drop_ptr(g, d, (ptrtype_t*)effective_type);
  } else if (bt->kind == TYPE_ARRAY) {
    gen_drop_array(g, d, (arraytype_t*)bt);
  }

  if (d->type->kind == TYPE_OPTIONAL) {
    g->indent--;
    startlinex(g);
    CHAR('}');
  }
}


static void gen_drops(cgen_t* g, const droparray_t* drops) {
  ptrarray_t* visitstack = &g->tmpptrarray;
  for (u32 i = 0; i < drops->len; i++) {
    visitstack->len = 0;
    gen_drop(g, visitstack, &drops->v[i]);
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
      return expr_contains_owners(((binop_t*)n)->left) ||
             expr_contains_owners(((binop_t*)n)->right);

    case EXPR_BLOCK: {
      const block_t* b = (const block_t*)n;
      for (u32 i = 0; i < b->children.len; i++) {
        if (expr_contains_owners((expr_t*)b->children.v[i]))
          return true;
      }
      return false;
    }

    case EXPR_CALL: {
      const call_t* call = (const call_t*)n;
      if (expr_contains_owners(call->recv))
        return true;
      for (u32 i = 0; i < call->args.len; i++) {
        if (expr_contains_owners((expr_t*)call->args.v[i]))
          return true;
      }
      return false;
    }

    case EXPR_SUBSCRIPT:
      return expr_contains_owners(((subscript_t*)n)->recv) ||
             expr_contains_owners(((subscript_t*)n)->index);

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


static void gen_drops_before_stmt(
  cgen_t* g, const droparray_t* drops, const void* node)
{
  if (drops->len) {
    gen_drops(g, drops);
    startline(g, ((const node_t*)node)->loc);
  } else {
    startline_if_needed(g, ((const node_t*)node)->loc);
  }
}


static void gen_block(cgen_t* g, const block_t* n) {
  g->scopenest++;

  if (n->flags & NF_RVALUE) {
    if (n->drops.len == 0) {
      // simplify empty expression block
      if (n->children.len == 0) {
        PRINT("((void)0)");
        g->scopenest--;
        return;
      }
      // simplify expression block with a single sub expression
      if (n->children.len == 1) {
        gen_expr_rvalue(g, (expr_t*)n->children.v[0], n->type);
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
    gen_type(g, n->type), CHAR(' '), PRINT(block_resvar), CHAR(';');
  }

  u32 start_lineno = g->lineno;
  g->indent++;
  char tmp[TMP_ID_SIZE];

  if (n->children.len > 0) {
    sizetuple_t startlens;
    for (u32 i = 0, last = n->children.len - 1; i <= last; i++) {
      const expr_t* cn = (expr_t*)n->children.v[i];

      // before returning we need to generate drops, however the return value
      // might use a local that is cleaned up, so we must generate drops _after_
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
        // return from function
        // e.g. "fun foo(x int) int { return x }"
        const retexpr_t* ret = (const retexpr_t*)cn;
        if (ret->type == type_void) {
          startline_if_needed(g, cn->loc);
          if (ret->value)
            gen_expr(g, ret->value), CHAR(';');
          gen_drops(g, &n->drops);
        } else if (n->drops.len && expr_contains_owners(cn)) {
          startline_if_needed(g, cn->loc);
          fmt_tmp_id(tmp, sizeof(tmp), ret);
          // "T tmp = expr;"
          gen_type(g, ret->type), PRINT(" const "), PRINT(tmp), PRINT(" = ");
          expr_or_zeroinit(g, ret->type, ret->value), CHAR(';');
          gen_drops(g, &n->drops);
          startlinex(g);
          PRINT("return "), PRINT(tmp), CHAR(';');
        } else {
          gen_drops_before_stmt(g, &n->drops, cn);
          gen_retexpr(g, (const retexpr_t*)cn, NULL), CHAR(';');
        }
        break;
      }

      if (i == last && block_isrvalue) {
        // result from rvalue block
        // e.g. "let x = { 1; 2 }" => "x" is an "int" with value "2"
        x_semi_begin(g, cn->loc);
        PRINT(block_resvar), PRINT(" = "), gen_expr(g, cn), CHAR(';');
        gen_drops(g, &n->drops);
        break;
      }

      // regular statement or expression
      startlens = x_semi_begin(g, cn->loc);
      gen_expr(g, cn);
      if ((cn->kind == EXPR_BLOCK || cn->kind == EXPR_IF) && lastchar(g) == '}')
        x_semi_cancel(&startlens);
      x_semi_end(g, startlens);

      if (i == last)
        gen_drops(g, &n->drops);
    } // for
    g->indent--;
    if (start_lineno != g->lineno) {
      startline(g, (loc_t){0});
    } else {
      CHAR(' ');
    }
  } else { // empty block
    if (n->drops.len > 0) {
      gen_drops(g, &n->drops);
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
    PRINTF(ANON_FMT, g->idgen_local++);
  }
}


static bool noalias(const type_t* t) {
  // PTR: pointers are always unique (one owner)
  // REF: there can only be one mutable ref at any given time
  return (
    t->kind == TYPE_PTR ||
    t->kind == TYPE_MUTREF
  );
}


static void gen_fun(cgen_t* g, const fun_t* fun) {
  assertf(fun->mangledname != NULL, "%s", fun->name);
  PRINT(fun->mangledname);
}


static void gen_fun_proto(cgen_t* g, const fun_t* fun) {
  funtype_t* ft = (funtype_t*)fun->type;

  switch (fun->flags & NF_VIS_MASK) {
    case NF_VIS_UNIT: PRINT("static "); break;
    case NF_VIS_PKG:  PRINT(CO_INTERNAL_PREFIX "pkg "); break;
    case NF_VIS_PUB:  PRINT(CO_INTERNAL_PREFIX "pub "); break;
  }

  gen_type(g, ft->result);
  CHAR(' ');
  PRINT(assertnotnull(fun->mangledname));
  CHAR('(');
  if (ft->params.len > 0) {
    g->scopenest++;
    for (u32 i = 0; i < ft->params.len; i++) {
      local_t* param = (local_t*)ft->params.v[i];
      if (i) PRINT(", ");
      // if (!type_isprim(param->type) && !param->ismut)
      //   PRINT("const ");
      gen_type(g, param->type);
      if (noalias(param->type))
        PRINT(CO_INTERNAL_PREFIX "noalias");
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


static void gen_fun_decl(cgen_t* g, const fun_t* fn) {
  // ignore pure declarations, e.g.
  //   fun foo(int) int             <—— declaration ignored
  //   fun foo(x int) int { x * 2 } <—— definition  included
  //   pub "C" bar(int) int         <—— declaration ignored
  if (fn->name == sym_main)
    g->mainfun = fn;
  if (!fn->body)
    return;
  startline(g, fn->loc);
  gen_fun_proto(g, fn);
  CHAR(';');
}


static void gen_fun_def(cgen_t* g, const fun_t* fn) {
  // if (type_isowner(((funtype_t*)fn->type)->result))
  //   PRINT("__attribute__((__return_typestate__(unconsumed))) ");
  if (fn->name == sym_main)
    g->mainfun = fn;
  assert(g->scopenest == 0);
  startline(g, fn->loc);
  gen_fun_proto(g, fn);
  if (fn->body) {
    CHAR(' ');
    g->idgen_local = 0;
    gen_block(g, fn->body);
  } else {
    CHAR(';');
  }
}


static void gen_structinit_field(cgen_t* g, const type_t* t, const expr_t* value) {
  if (t->kind == TYPE_OPTIONAL && !type_isptrlike(((const opttype_t*)t)->elem)) {
    PRINT("{.ok=1,.v="); gen_expr(g, value); CHAR('}');
  } else {
    gen_expr(g, value);
  }
}


static void gen_structinit(cgen_t* g, const structtype_t* t, nodearray_t args) {
  assert(args.len <= t->fields.len);
  CHAR('{');
  u32 i = 0;
  for (; i < args.len; i++) {
    const expr_t* arg = (expr_t*)args.v[i]; assert(nodekind_isexpr(arg->kind));
    const local_t* f = (local_t*)t->fields.v[0];
    if (arg->kind == EXPR_PARAM)
      break;
    if (i) PRINT(", ");
    gen_structinit_field(g, f->type, arg);
  }

  if (i == args.len && !t->hasinit) {
    if (i == 0 && t->fields.len > 0) {
      const local_t* f = (local_t*)t->fields.v[0];
      gen_zeroinit(g, f->type);
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
    const local_t* f = (local_t*)t->fields.v[i];
    const void** vp = (const void**)map_assign_ptr(initmap, g->ma, f->name);
    if UNLIKELY(!vp)
      return seterr(g, ErrNoMem);
    *vp = f;
  }

  // generate named arguments
  for (; i < args.len; i++) {
    if (i) PRINT(", ");
    const local_t* arg = (local_t*)args.v[i];
    CHAR('.'); PRINT(arg->name); CHAR('=');
    const local_t** fp = (const local_t**)map_lookup_ptr(initmap, arg->name);
    assert(fp && *fp);
    gen_structinit_field(g, (*fp)->type, assertnotnull(arg->init));
    map_del_ptr(initmap, arg->name);
  }

  // generate remaining fields with non-zero initializers
  // FIXME TODO: sort
  for (const mapent_t* e = map_it(initmap); map_itnext(initmap, &e); ) {
    const local_t* f = e->value;
    if (f->init) {
      if (i) PRINT(", ");
      CHAR('.'); PRINT(f->name); CHAR('=');
      gen_structinit_field(g, (f)->type, assertnotnull(f->init));
      i++; // for ", "
    }
  }

  CHAR('}');
}


static void gen_zeroinit_array(cgen_t* g, const arraytype_t* t) {
  if (t->len == 0) {
    // runtime dynamic array "{ cap, len uint; T* ptr }"
    PRINT("{0}");
    return;
  }
  // (u32[5]){0u}
  CHAR('(');
  gen_type(g, t->elem);
  PRINTF("[%llu])", t->len);
  CHAR('{');
  gen_zeroinit(g, t->elem);
  CHAR('}');
}


static void gen_zeroinit(cgen_t* g, const type_t* t) {
  t = unwind_aliastypes(t);
again:
  switch (t->kind) {
  case TYPE_VOID:
    CHAR('0');
    break;
  case TYPE_BOOL:
    PRINT("false");
    break;
  case TYPE_UINT:
    t = g->compiler->uinttype;
    goto again;
  case TYPE_INT:
    t = g->compiler->inttype;
    goto again;
  case TYPE_I32:
    CHAR('0');
    break;
  case TYPE_U32:
    PRINT("0u");
    break;
  case TYPE_I64:
    PRINT("0ll");
    break;
  case TYPE_U64:
    PRINT("0llu");
    break;
  case TYPE_I8:
  case TYPE_U8:
  case TYPE_I16:
  case TYPE_U16:
    CHAR('('); CHAR('('); gen_type(g, t); PRINT(")0)");
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
  case TYPE_SLICE:
  case TYPE_MUTSLICE:
    PRINT("{0}");
    break;
  case TYPE_ARRAY:
    return gen_zeroinit_array(g, (arraytype_t*)t);
  case TYPE_PTR:
    PRINT("NULL");
    break;
  case TYPE_STRUCT:
    gen_structinit(g, (structtype_t*)t, (nodearray_t){0});
    break;
  default:
    debugdie(g, t, "unexpected type %s", nodekind_name(t->kind));
  }
  // PRINTF(";memset(&%s,0,%zu)", n->name, n->type->size);
}


static void gen_primtype_cast(cgen_t* g, const type_t* t, const expr_t* nullable val) {
  const type_t* basetype = unwind_aliastypes(t);

  // skip redundant "(T)v" when v is T
  if (val && (val->type == t || val->type == basetype))
    return gen_expr_rvalue(g, val, t);

  CHAR('('); gen_type(g, t); CHAR(')');

  if (val) {
    gen_expr_rvalue(g, val, t);
  } else {
    gen_zeroinit(g, t);
  }
}


static void gen_call_type(cgen_t* g, const call_t* n, const type_t* t) {
  if (type_isprim(t)) {
    assert(n->args.len < 2);
    return gen_primtype_cast(g, t, n->args.len ? (expr_t*)n->args.v[0] : NULL);
  }

  CHAR('('); gen_type(g, t); CHAR(')');

  switch (t->kind) {
  case TYPE_STRUCT:
    gen_structinit(g, (const structtype_t*)t, n->args);
    break;
  default:
    dlog("NOT IMPLEMENTED: type call %s", nodekind_name(t->kind));
    error(g, t, "NOT IMPLEMENTED: type call %s", nodekind_name(t->kind));
  }
}


static void gen_call_fn_recv(
  cgen_t* g, const call_t* n, expr_t** selfp, bool* isselfrefp)
{
  switch (n->recv->kind) {

  case EXPR_MEMBER: {
    member_t* m = (member_t*)n->recv;
    fun_t* fn = (fun_t*)m->target;
    if (fn->kind != EXPR_FUN)
      break;
    funtype_t* ft = (funtype_t*)fn->type;
    if (ft->params.len > 0 && ((const local_t*)ft->params.v[0])->isthis) {
      const local_t* thisparam = (local_t*)ft->params.v[0];
      *isselfrefp = type_isref(thisparam->type);
      *selfp = m->recv;
    }
    assert(fn->name != sym__);
    gen_fun(g, fn);
    return;
  }

  case EXPR_ID: {
    const idexpr_t* id = (const idexpr_t*)n->recv;
    if (id->ref->kind == EXPR_FUN)
      return gen_fun(g, (fun_t*)id->ref);
    break;
  }

  case EXPR_FUN:
    return gen_fun(g, (fun_t*)n->recv);

  }
  gen_expr(g, n->recv);
}


static void gen_call_fun(cgen_t* g, const call_t* n) {
  // okay, then it must be a function call
  assert(n->recv->type->kind == TYPE_FUN);
  const funtype_t* ft = (funtype_t*)n->recv->type;

  // owner sink? (i.e. return value is unused but must be dropped)
  bool owner_sink = (n->flags & NF_RVALUE) == 0 && type_isowner(n->type);
  if (owner_sink)
    drop_begin(g, (expr_t*)n);

  // recv
  expr_t* self = NULL;
  bool isselfref = false;
  gen_call_fn_recv(g, n, &self, &isselfref);

  // args
  CHAR('(');
  if (self) {
    if (isselfref && !type_isref(self->type))
      CHAR('&');
    gen_expr(g, self);
    if (n->args.len > 0)
      PRINT(", ");
  }
  for (u32 i = 0; i < n->args.len; i++) {
    if (i) PRINT(", ");
    const expr_t* arg = (expr_t*)n->args.v[i];
    if (arg->kind == EXPR_PARAM) // named argument
      arg = ((local_t*)arg)->init;
    const type_t* dst_t = ((local_t*)ft->params.v[i])->type;
    gen_expr_rvalue(g, arg, dst_t);
  }
  CHAR(')');

  if (owner_sink)
    drop_end(g);
}


static void gen_call(cgen_t* g, const call_t* n) {
  // type call?
  const idexpr_t* idrecv = (idexpr_t*)n->recv;
  if (idrecv->kind == EXPR_ID && nodekind_istype(idrecv->ref->kind))
    return gen_call_type(g, n, (type_t*)idrecv->ref);
  if (nodekind_istype(n->recv->kind))
    return gen_call_type(g, n, (type_t*)n->recv);

  // okay, then it must be a function call
  return gen_call_fun(g, n);
}


static void gen_typecons(cgen_t* g, const typecons_t* n) {
  assert(type_isprim(unwind_aliastypes(n->type)));
  return gen_primtype_cast(g, n->type, n->expr);
}


static void gen_binop(cgen_t* g, const binop_t* n) {
  gen_expr_rvalue(g, n->left, n->left->type);
  CHAR(' ');
  PRINT(operator(n->op));
  CHAR(' ');
  gen_expr_rvalue(g, n->right, n->right->type);
}


static void gen_intconst(cgen_t* g, u64 value, const type_t* t) {
  if (t->kind < TYPE_I32)
    CHAR('('), gen_type(g, t), CHAR(')');

  if (!type_isunsigned(t) && (value & 0x1000000000000000) ) {
    value &= ~0x1000000000000000;
    CHAR('-');
  }
  u32 base = value >= 1024 ? 16 : 10;
  if (base == 16)
    PRINT("0x");
  buf_print_u64(&g->outbuf, value, base);
again:
  switch (t->kind) {
    case TYPE_INT:  t = g->compiler->inttype; goto again;
    case TYPE_UINT: t = g->compiler->uinttype; goto again;
    case TYPE_I64:  PRINT("ll"); break;
    case TYPE_U64:  PRINT("llu"); break;
    case TYPE_U32:  CHAR('u'); break;
  }
}


static void gen_intlit(cgen_t* g, const intlit_t* n) {
  gen_intconst(g, n->intval, n->type);
}


static void gen_floatlit(cgen_t* g, const floatlit_t* n) {
  PRINTF("%f", n->f64val);
  if (n->type->kind == TYPE_F32)
    CHAR('f');
}


static void gen_boollit(cgen_t* g, const intlit_t* n) {
  PRINT(n->intval ? "true" : "false");
}


static void gen_strlit(cgen_t* g, const strlit_t* n) {
  const type_t* t = n->type;
  if (type_isref(t) && ((reftype_t*)t)->elem->kind == TYPE_ARRAY) {
    PRINTF("(const u8[%llu]){\"", n->len);
    buf_appendrepr(&g->outbuf, n->bytes, n->len);
    PRINT("\"}");
  } else {
    // string, alias for &[u8]
    // t = unwind_aliastypes(t);
    // const slicetype_t* st = (slicetype_t*)t;
    assert(type_isslice(unwind_aliastypes(t)));
    PRINT("(");
    gen_type(g, t);
    PRINTF("){%llu,(const u8[%llu]){\"", n->len, n->len);
    buf_appendrepr(&g->outbuf, n->bytes, n->len);
    PRINT("\"}}");
  }
}


static void gen_arraylit1(cgen_t* g, const arraylit_t* n, u64 len) {
  const arraytype_t* at = (arraytype_t*)n->type;
  CHAR('(');
  // PRINT("const "); // TODO: track constctx
  gen_type(g, at->elem);
  PRINTF("[%llu]){", len);
  for (u32 i = 0; i < n->values.len; i++) {
    if (i) CHAR(',');
    gen_expr(g, (expr_t*)n->values.v[i]);
  }
  PRINT("}");
}


static void gen_dyn_arraylit(cgen_t* g, const arraylit_t* n) {
  // see gen_darray_typedef
  const arraytype_t* at = (arraytype_t*)n->type;

  // note: potential overflow of size already checked by typecheck
  //u8 elemalign = at->elem->align;
  u64 elemsize = at->elem->size;
  u64 len = n->values.len;

  CHAR('('), gen_type(g, (type_t*)at), CHAR(')');
  PRINTF("{%llu,%llu,", len, len);
  PRINT(CO_INTERNAL_PREFIX "mem_dup(&");
  gen_arraylit1(g, n, n->values.len);
  PRINTF(",%llu)}", len * elemsize);
}


static void gen_arraylit(cgen_t* g, const arraylit_t* n) {
  const arraytype_t* at = (arraytype_t*)n->type;
  if (at->len == 0)
    return gen_dyn_arraylit(g, n);
  gen_arraylit1(g, n, at->len);
}


static void gen_assign(cgen_t* g, const binop_t* n) {
  if UNLIKELY(n->left->kind == EXPR_ID && ((idexpr_t*)n->left)->name == sym__) {
    // "_ = expr" => "expr"  if expr may have side effects or building in debug mode
    // "_ = expr" => ""      if expr does not have side effects
    // note: this may cause a warning to be printed by cc (in DEBUG builds only)
    if (g->compiler->buildmode == BUILDMODE_DEBUG || !expr_no_side_effects(n->right))
      gen_expr_rvalue(g, n->right, n->type);
    return;
  }
  gen_expr(g, n->left);
  CHAR(' ');
  PRINT(operator(n->op));
  CHAR(' ');
  gen_expr_rvalue(g, n->right, n->type);
}


static void gen_vardef1(
  cgen_t* g, const local_t* n, const char* name, bool wrap_rvalue)
{
  // elide unused variable without side effects.
  // Note: This isn't very useful in practice as clang will optimize away
  // unused code anyway (when optimizing), but it's a nice thing to do.
  if (n->nuse == 0 &&
      #if DEBUG
      // disable for -d during in debug builds of compis itself
      g->compiler->buildmode != BUILDMODE_DEBUG &&
      #endif
      expr_no_side_effects((expr_t*)n))
  {
    PRINT("/* elided unused ");
    PRINT(nodekind_fmt(n->kind));
    CHAR(' ');
    if (name[0] != '_' || (name[1] && !string_startswith(name, CO_INTERNAL_PREFIX))) {
      PRINT(name);
    } else {
      // catch all "_" and "{CO_INTERNAL_PREFIX}..." vars
      CHAR('_');
    }
    PRINT(" */");
    return;
  }

  if ((n->flags & NF_RVALUE) && wrap_rvalue)
    PRINT("({");

  gen_type(g, n->type);

  // "const" qualifier for &T that's a pointer
  if (n->kind == EXPR_LET &&
    ( type_isprim(n->type) ||
      n->type->kind == TYPE_OPTIONAL ||
      ( type_isref(n->type) && !reftype_byvalue(g, (reftype_t*)n->type) )
    )
  ){
    PRINT(" const");
  }
  CHAR(' ');
  id(g, name);

  if (n->nuse == 0)
    CHAR(' '), PRINT(CO_INTERNAL_PREFIX "unused");

  // if (type_isptr(n->type)) {
  //   PRINTF(" __attribute__((__consumable__(%s)))",
  //     owner_islive(n) ? "unconsumed" : "consumed");
  // }

  PRINT(" = ");

  if (n->init) {
    if (n->type->kind == TYPE_OPTIONAL && n->init->type->kind != TYPE_OPTIONAL) {
      gen_optinit(g, n->init, /*isshort*/true);
      //gen_optinit(g, ((const opttype_t*)n->type)->elem, n->init, /*isshort*/true);
    } else {
      gen_expr_rvalue(g, n->init, n->type);
    }
  } else {
    gen_zeroinit(g, n->type);
  }

  if ((n->flags & NF_RVALUE) && wrap_rvalue)
    PRINT("; "), PRINT(name), PRINT(";})");
}


static void gen_vardef(cgen_t* g, const local_t* n) {
  gen_vardef1(g, n, n->name, true);
}


static void gen_deref(cgen_t* g, const unaryop_t* n) {
  const type_t* t = n->expr->type;
  if (t->kind == TYPE_REF && reftype_byvalue(g, (reftype_t*)t)) {
    gen_expr(g, n->expr);
  } else {
    CHAR('*');
    // note: must not uese expr_rvalue here since it does implicit deref
    bool parenwrap = has_ambiguous_prec(n->expr);
    if (parenwrap)
      CHAR('(');
    gen_expr(g, n->expr);
    if (parenwrap)
      CHAR(')');
  }
}


static void gen_doref(cgen_t* g, const unaryop_t* n) {
  // e.g. "&x"
  switch (n->expr->type->kind) {
    case TYPE_ARRAY:
      // nothing to do; array is already a pointer
      break;
    default:
      panic("TODO ref to expr `%s` of type kind %s",
        fmtnode(g, 0, n->expr), nodekind_name(n->expr->type->kind));
  }
  gen_expr_rvalue(g, n->expr, n->type);
}


static void gen_prefixop(cgen_t* g, const unaryop_t* n) {
  if (n->op == OP_REF || n->op == OP_MUTREF)
    return gen_doref(g, n);
  if (n->expr->kind == EXPR_INTLIT && n->expr->type->kind < TYPE_I32)
    CHAR('('), gen_type(g, n->expr->type), CHAR(')');
  PRINT(operator(n->op));
  gen_expr_rvalue(g, n->expr, n->type);
}


static void gen_postfixop(cgen_t* g, const unaryop_t* n) {
  gen_expr_rvalue(g, n->expr, n->type);
  PRINT(operator(n->op));
}


static void gen_idexpr(cgen_t* g, const idexpr_t* n) {
  id(g, n->name);
}


static void gen_param(cgen_t* g, const local_t* n) {
  id(g, n->name);
}


static void gen_nsexpr(cgen_t* g, const nsexpr_t* n) {
  // Note: maybe don't do anything since a namespace is not materialized (it's abstract)
  panic("TODO namespace");
}


static void gen_member_op(cgen_t* g, const type_t* recvt) {
  if (recvt->kind == TYPE_PTR ||
    (type_isref(recvt) && !reftype_byvalue(g, (reftype_t*)recvt)))
  {
    PRINT("->");
  } else {
    CHAR('.');
  }
}


static void gen_member(cgen_t* g, const member_t* n) {
  // TODO: nullcheck doesn't work for assignments, e.g. "foo->ptr = ptr"
  // bool insert_nullcheck = n->type->kind == TYPE_REF || n->type->kind == TYPE_FUN;
  bool insert_nullcheck = false;
  if (insert_nullcheck) {
    PRINT(CO_INTERNAL_PREFIX "checknull(");
    gen_expr(g, n->recv);
  } else {
    gen_expr_rvalue(g, n->recv, n->recv->type);
  }
  if (n->recv->type->kind == TYPE_OPTIONAL) {
    panic("TODO optional access!");
  }
  gen_member_op(g, n->recv->type);
  PRINT(n->name);
  if (insert_nullcheck)
    CHAR(')');
}


static void gen_subscript(cgen_t* g, const subscript_t* n) {
  // If no bounds checking is needed, this is simply one of the following:
  //
  //   recv[index]      -- for arrays of known size
  //   recv.ptr[index]  -- for slices and dynamic arrays
  //
  // Otherwise one of the following are generated for slices and dynamic arrays,
  // depending on idempotency of the recv and index:
  //
  //   ( __co_checkbounds(recv.len,index), recv.ptr[index] )
  //
  //   ({ __co_t_slice_s v1 = recv; u64 v2 = index;
  //      __co_checkbounds(v1.len,v2); v1.ptr[v2]; })
  //
  //   ({ __co_t_slice_s v1 = recv; __co_checkbounds(v1.len,2); v1.ptr[2]; })
  //
  //   ({ u64 v1 = index; __co_checkbounds(recv.len,v1); recv.ptr[v1]; })
  //
  // For arrays of known size, e.g. "[int 3]":
  //
  //   ( __co_checkbounds(3,index), recv[index] )
  //   ({ u64 v1 = index; __co_checkbounds(3,v1);  a[v1]; })
  //
  bool checkbounds = false;
  char len_buf[16] = {0};
  const char* ptr_code = ".ptr";

  type_t* recv_type = unwrap_ptr_and_alias(n->recv->type);

  switch (recv_type->kind) {
    case TYPE_SLICE:
    case TYPE_MUTSLICE:
      checkbounds = true;
      break;
    case TYPE_ARRAY:
      if (((arraytype_t*)recv_type)->len) {
        ptr_code = "";
        if (!(n->index->flags & NF_CONST)) {
          checkbounds = true;
          sfmtu64(len_buf, ((arraytype_t*)recv_type)->len, 10);
          // note: len_buf is zeroed; we don't need to explicitly terminate it
        }
      } else {
        checkbounds = true;
      }
      break;
    default:
      panic("TODO subscript recv type %s", nodekind_name(n->recv->type->kind));
  }

  u32 index_tmp_id = 0;
  u32 recv_tmp_id = 0;

  if (checkbounds) {
    if (n->recv->kind != EXPR_ID)
      recv_tmp_id = g->idgen_local++;
    if (!(n->index->flags & NF_CONST) && n->index->kind != EXPR_ID)
      index_tmp_id = g->idgen_local++;

    CHAR('(');
    if (index_tmp_id || recv_tmp_id)
      CHAR('{');

    if (recv_tmp_id) {
      gen_type(g, n->recv->type);
      PRINTF(" " ANON_FMT " = ", recv_tmp_id);
      gen_expr_rvalue(g, n->recv, n->recv->type);
      PRINT(";\n");
    }

    if (index_tmp_id) {
      gen_type(g, n->index->type);
      PRINTF(" " ANON_FMT " = ", index_tmp_id);
      gen_expr_rvalue(g, n->index, n->index->type);
      PRINT(";\n");
    }

    PRINT(CO_INTERNAL_PREFIX "checkbounds(");
    if (len_buf[0]) {
      PRINT(len_buf);
    } else {
      if (recv_tmp_id) {
        PRINTF(ANON_FMT, recv_tmp_id);
      } else {
        gen_expr_rvalue(g, n->recv, n->recv->type);
      }
      PRINT(".len");
    }
    CHAR(',');
    if (index_tmp_id) {
      PRINTF(ANON_FMT, index_tmp_id);
    } else if (n->index->flags & NF_CONST) {
      PRINTF("%llu", n->index_val);
    } else {
      gen_expr_rvalue(g, n->index, n->index->type);
    }
    CHAR(')');

    CHAR((index_tmp_id || recv_tmp_id) ? ';' : ',');

    if (recv_tmp_id) {
      PRINTF(ANON_FMT, recv_tmp_id);
    } else {
      gen_expr_rvalue(g, n->recv, n->recv->type);
    }
  } else {
    gen_expr_rvalue(g, n->recv, n->recv->type);
  }

  PRINT(ptr_code);

  CHAR('[');
  if (index_tmp_id) {
    PRINTF(ANON_FMT, index_tmp_id);
  } else if (n->index->flags & NF_CONST) {
    PRINTF("%llu", n->index_val);
  } else {
    gen_expr_rvalue(g, n->index, n->index->type);
  }
  CHAR(']');

  if (index_tmp_id || recv_tmp_id)
    PRINT(";}");
  if (checkbounds)
    CHAR(')');
}


static void gen_expr_in_block(cgen_t* g, const expr_t* n) {
  if (n->kind == EXPR_BLOCK || n->kind == EXPR_IF)
    return gen_expr(g, n);
  PRINT("{ ");
  gen_expr(g, n);
  PRINT("; }");
}


static void gen_ifexpr(cgen_t* g, const ifexpr_t* n) {
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
      gen_vardef1(g, var, var->name, false);
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
        CHAR('('), gen_expr_rvalue(g, var->init, var->init->type), PRINT(".ok)");
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
      gen_opttype(g, &t);
      PRINT(" const "), PRINT(tmp), PRINT(" = ");
      gen_expr_rvalue(g, var->init, (type_t*)&t);
      CHAR(';');

      // "K x = tmp.v;"
      startline(g, var->loc);
      gen_type(g, var->type);
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
      // e.g.
      //   let x ?int
      //   if x {
      //     let y int = x
      //   }
      id = (const idexpr_t*)n->cond;
      if (assertnotnull(id->ref)->nuse > 0) {
        g->indent++;
        if (n->flags & NF_RVALUE)
          CHAR('(');
        PRINT("{ ");
        gen_opttype(g, (const opttype_t*)n->cond->type);
        fmt_tmp_id(tmp, sizeof(tmp), id);
        PRINTF(" %s = %s; ", tmp, id->name);

        // "T x = tmp.v;"
        gen_type(g, ((const opttype_t*)n->cond->type)->elem);
        PRINTF(" %s = %s.v; ", id->name, tmp);
      } else {
        has_tmp_opt = false;
      }
    }

    if (n->flags & NF_RVALUE) {
      if (id) {
        PRINTF("(%s.ok ? ", has_tmp_opt ? tmp : id->name);
      } else {
        CHAR('('), gen_expr_rvalue(g, n->cond, n->cond->type);
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
        PRINT("if ("), gen_expr_rvalue(g, n->cond, n->cond->type), CHAR(')');
      }
    }
  }

  if (n->flags & NF_RVALUE) {
    startline(g, n->thenb->loc);

    if ((!n->elseb || n->elseb->type == type_void) && !type_isopt(n->thenb->type)) {
      gen_optinit(g, (expr_t*)n->thenb, /*isshort*/false);
    } else {
      gen_block(g, n->thenb);
    }
    PRINT(" : (");
    if (n->elseb) {
      if (loc_line(n->elseb->loc) != g->lineno)
        startline(g, n->elseb->loc);
      gen_block(g, n->elseb);
      if (n->elseb->type == type_void)
        PRINT(", ");
    } else {
      CHAR(' ');
    }
    if (!n->elseb || n->elseb->type == type_void) {
      type_t* elem = n->thenb->type;
      if (type_isopt(elem))
        elem = ((const opttype_t*)elem)->elem;
      gen_optzero(g, elem, /*isshort*/false);
    }
    PRINT("))");
  } else {
    gen_block(g, n->thenb);
    if (n->elseb) {
      if (lastchar(g) != '}')
        CHAR(';'); // terminate non-block "then" body
      PRINT(" else ");
      gen_block(g, n->elseb);
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


static void gen_forexpr(cgen_t* g, const forexpr_t* n) {
  PRINT("for (");
  if (n->start)
    gen_expr(g, n->start);
  PRINT("; ");
  gen_expr(g, n->cond);
  PRINT("; ");
  if (n->end)
    gen_expr(g, n->end);
  PRINT(") ");
  gen_expr_in_block(g, n->body);
}


static void gen_expr(cgen_t* g, const expr_t* n) {
  switch ((enum nodekind)n->kind) {
  case EXPR_FUN:       return gen_fun(g, (const fun_t*)n);
  case EXPR_INTLIT:    return gen_intlit(g, (const intlit_t*)n);
  case EXPR_FLOATLIT:  return gen_floatlit(g, (const floatlit_t*)n);
  case EXPR_BOOLLIT:   return gen_boollit(g, (const intlit_t*)n);
  case EXPR_STRLIT:    return gen_strlit(g, (const strlit_t*)n);
  case EXPR_ARRAYLIT:  return gen_arraylit(g, (const arraylit_t*)n);
  case EXPR_ID:        return gen_idexpr(g, (const idexpr_t*)n);
  case EXPR_NS:        return gen_nsexpr(g, (const nsexpr_t*)n);
  case EXPR_PARAM:     return gen_param(g, (const local_t*)n);
  case EXPR_BLOCK:     return gen_block(g, (const block_t*)n);
  case EXPR_CALL:      return gen_call(g, (const call_t*)n);
  case EXPR_TYPECONS:  return gen_typecons(g, (const typecons_t*)n);
  case EXPR_MEMBER:    return gen_member(g, (const member_t*)n);
  case EXPR_SUBSCRIPT: return gen_subscript(g, (const subscript_t*)n);
  case EXPR_IF:        return gen_ifexpr(g, (const ifexpr_t*)n);
  case EXPR_FOR:       return gen_forexpr(g, (const forexpr_t*)n);
  case EXPR_RETURN:    return gen_retexpr(g, (const retexpr_t*)n, NULL);
  case EXPR_DEREF:     return gen_deref(g, (const unaryop_t*)n);
  case EXPR_PREFIXOP:  return gen_prefixop(g, (const unaryop_t*)n);
  case EXPR_POSTFIXOP: return gen_postfixop(g, (const unaryop_t*)n);
  case EXPR_BINOP:     return gen_binop(g, (const binop_t*)n);
  case EXPR_ASSIGN:    return gen_assign(g, (const binop_t*)n);

  case EXPR_VAR:
  case EXPR_LET:
    return gen_vardef(g, (const local_t*)n);

  // node types we should never see
  case NODE_BAD:
  case NODE_COMMENT:
  case NODE_UNIT:
  case NODE_IMPORTID:
  case NODE_TPLPARAM:
  case NODE_FWDDECL:
  case STMT_TYPEDEF:
  case STMT_IMPORT:
  case EXPR_FIELD:
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
  case TYPE_ARRAY:
  case TYPE_SLICE:
  case TYPE_MUTSLICE:
  case TYPE_FUN:
  case TYPE_PTR:
  case TYPE_REF:
  case TYPE_MUTREF:
  case TYPE_OPTIONAL:
  case TYPE_STRUCT:
  case TYPE_ALIAS:
  case TYPE_NS:
  case TYPE_TEMPLATE:
  case TYPE_PLACEHOLDER:
  case TYPE_UNKNOWN:
  case TYPE_UNRESOLVED:
    break;
  }
  debugdie(g, n, "unexpected node %s", nodekind_name(n->kind));
}


static void gen_imports(cgen_t* g, const unit_t* unit) {
  if (g->pkg->imports.len == 0)
    return;

  // build list of packages that this unit needs
  pkg_t** depv = mem_alloc(g->ma, sizeof(void*) * (usize)g->pkg->imports.len).p;
  if (!depv) {
    g->err = ErrNoMem;
    return;
  }
  u32 depc = 0;
  bool include_stdruntime = !g->compiler->opt_nostdruntime;

  for (const import_t* im = unit->importlist; im; ) {
    pkg_t* pkg = assertnotnull(im->pkg);
    for (u32 i = depc; i;) {
      if (depv[--i] == pkg)
        goto next;
    }
    depv[depc++] = pkg;
    if (include_stdruntime && pkg == g->compiler->stdruntime_pkg) {
      // package explicitly imports std/runtime
      include_stdruntime = false;
    }
  next:
    im = im->next_import;
  }

  str_t headerfile = {0};

  // include API headers for std/runtime
  if (include_stdruntime) {
    assertnotnull(g->compiler->stdruntime_pkg); // should have been loaded by pkgbuild
    if UNLIKELY(!pkg_buildfile(
        g->compiler->stdruntime_pkg, g->compiler, &headerfile, PKG_APIHFILE_NAME))
    {
      g->err = ErrNoMem;
      goto end;
    }
    PRINTF("#include \"%s\"\n", headerfile.p);
  }

  // include API headers for each imported package
  for (u32 i = 0; i < depc; i++) {
    headerfile.len = 0;
    if UNLIKELY(!pkg_buildfile(depv[i], g->compiler, &headerfile, PKG_APIHFILE_NAME)) {
      g->err = ErrNoMem;
      goto end;
    }
    PRINTF("// import \"%s\"\n", depv[i]->path.p);
    PRINTF("#include \"%s\"\n", headerfile.p);
  }

end:
  str_free(headerfile);
  mem_freex(g->ma, MEM(depv, sizeof(void*) * (usize)g->pkg->imports.len));
}


static void gen_main(cgen_t* g) {
  assertnotnull(g->mainfun);
  assertnotnull(g->mainfun->mangledname);
  PRINTF(
    "\n"
    "\n"
    "#line 0 \"<builtin>\"\n"
    "int main(int argc, char* argv[]) {\n"
    "  return %s(), 0;\n"
    "}",
    g->mainfun->mangledname
  );
}


static err_t finalize(cgen_t* g, usize headstart) {
  if (g->err)
    return g->err;
  if (g->headbuf.len > 0)
    buf_insert(&g->outbuf, headstart, g->headbuf.p, g->headbuf.len);
  // make sure outputs ends with LF
  if (g->outbuf.len > 0 && g->outbuf.chars[g->outbuf.len-1] != '\n')
    CHAR('\n');
  buf_nullterm(&g->outbuf);
  if (g->outbuf.oom)
    seterr(g, ErrNoMem);
  return g->err;
}


#ifdef DEBUG
  UNUSED static void dlog_defs(cgen_t* g, node_t** defv, u32 defc, const char* prefix) {
    for (u32 i = 0; i < defc; i++) {
      const node_t* n = defv[i];
      if (n->kind == NODE_FWDDECL) {
        const node_t* d = ((fwddecl_t*)n)->decl;
        dlog("%s%s#%p -> %s#%p %s", prefix,
          nodekind_name(n->kind), n,
          nodekind_name(d->kind), d, fmtnode(g, 1, d));
      } else {
        dlog("%s%s#%p %s%s", prefix,
          nodekind_name(n->kind), n, fmtnode(g, 0, n),
          (n->flags & NF_CYCLIC) ? " (cyclic)" : "");
      }
    }
  }
#else
  #define dlog_defs(...) ((void)0)
#endif


static void assign_mangledname(cgen_t* g, node_t* n) {
  char** p;

  if (n == (node_t*)&g->compiler->strtype) {
    // note: strtype has predefined mangledname.
    // it also has no nsparent, which compiler_mangle requires
    return;
  }

  switch (n->kind) {
    case EXPR_FUN:
      p = &((fun_t*)n)->mangledname;
      break;

    // usertype_t
    case TYPE_ARRAY:
      if (((arraytype_t*)n)->len > 0) // statically-sized array is just T*
        return;
      FALLTHROUGH;
    case TYPE_SLICE:
    case TYPE_MUTSLICE:
    case TYPE_OPTIONAL:
    case TYPE_STRUCT:
    case TYPE_ALIAS:
    case TYPE_NS:
      p = &((usertype_t*)n)->mangledname;
      break;

    // has no mangledname
    #ifdef DEBUG
    case NODE_FWDDECL:
    case TYPE_FUN:
    case TYPE_PTR:
    case TYPE_REF:
    case TYPE_MUTREF:
    case TYPE_TEMPLATE:
    case TYPE_PLACEHOLDER:
    case TYPE_UNRESOLVED:
      return;
    default:
      panic("TODO " REPRNODE_FMT, REPRNODE_ARGS(n, 0));
    #endif
  }
  assertf(*p == NULL, REPRNODE_FMT, REPRNODE_ARGS(n, 0));
  *p = mangle(g, n);
  //dlog("%s " REPRNODE_FMT " => %s", __FUNCTION__, REPRNODE_ARGS(n, 0), *p);
}


static void gen_fwddecl(cgen_t* g, const fwddecl_t* d, bool is_impl) {
  // if (d->decl->kind != EXPR_FUN)
  //   return;

  const node_t* n = assertnotnull(d->decl);

  switch (n->kind) {
    case EXPR_FUN:
      if (is_impl) {
        startline(g, n->loc);
        gen_fun_proto(g, (fun_t*)n);
        CHAR(';');
      }
      break;
    case TYPE_STRUCT: // "struct foo;"
      // if (is_impl) {
      //   startline(g, n->loc);
      //   gen_structtype(g, (structtype_t*)n);
      //   CHAR(';');
      // }
      break;
    case TYPE_ALIAS: // "typedef other alias;"
      gen_aliastype_def(g, (aliastype_t*)n);
      break;
    case TYPE_ARRAY: // "struct __co_array_abc;"
      if (((arraytype_t*)n)->len == 0) {
        startline(g, n->loc);
        gen_arraytype(g, (arraytype_t*)n);
        CHAR(';');
      }
      break;
    default:
      debugdie(g, n, "unexpected node %s", nodekind_name(n->kind));
  }
}


// gen_def generates declaration or definition of n
static void gen_def(cgen_t* g, const node_t* n, bool is_impl) {
  // don't generate templates
  if (n->flags & NF_TEMPLATE)
    return;

  // don't generate any code for partial template instances, e.g.
  //   type Template<A,B>
  //     x A
  //     y B
  //   type Partial<B>
  //     z Template<int,B>  <—— partial template instance
  //
  if (nodekind_isusertype(n->kind)) {
    usertype_t* ut = (usertype_t*)n;
    for (u32 i = 0; i < ut->templateparams.len; i++)
      if (ut->templateparams.v[i]->kind == TYPE_PLACEHOLDER)
        return;
  }

  trace("gen_def (%s) " REPRNODE_FMT, is_impl ? "impl" : "api", REPRNODE_ARGS(n, 0));

  switch (n->kind) {

  case NODE_FWDDECL:
    gen_fwddecl(g, (fwddecl_t*)n, is_impl);
    break;

  case STMT_TYPEDEF:
    MUSTTAIL return gen_def(g, (node_t*)((typedef_t*)n)->type, is_impl);

  case EXPR_FUN:
    if (is_impl)
      return gen_fun_def(g, (fun_t*)n);
    return gen_fun_decl(g, (fun_t*)n);

  case TYPE_STRUCT:
    gen_structtype_def(g, (structtype_t*)n);
    break;

  case TYPE_OPTIONAL:
    gen_opttype_def(g, (opttype_t*)n);
    break;

  case TYPE_FUN:
    // TODO: determine when a function type is actually used,
    // e.g. as a function parameter type or array-type element.
    //startline(g, n->loc);
    //PRINT("typedef "), gen_funtype(g, (funtype_t*)n, NULL), CHAR(';');
    break;

  case TYPE_ARRAY:
    gen_arraytype_def(g, (arraytype_t*)n);
    break;

  case TYPE_ALIAS:
    gen_aliastype_def(g, (aliastype_t*)n);
    break;

  case TYPE_SLICE:
  case TYPE_MUTSLICE:
    gen_slicetype_def(g, (slicetype_t*)n);
    break;

  // nothing to be done for these
  case TYPE_PTR:
  case TYPE_REF:
  case TYPE_MUTREF:
  case TYPE_TEMPLATE:
    break;

  default:
    debugdie(g, n, "unexpected node %s", nodekind_name(n->kind));
  }
}


static isize nodearray_indexof(const nodearray_t* na, const node_t* n) {
  assert((u64)na->len <= (u64)ISIZE_MAX);
  for (u32 i = 0; i < na->len; i++) {
    if (na->v[i] == n)
      return (isize)i;
  }
  return -1;
}


static void gen_unit(cgen_t* g, unit_t* unit, cgen_pkgapi_t* pkgapi) {
  assert_nodekind(unit, NODE_UNIT);
  nodearray_t children = unit->children;

  if (children.len == 0)
    return;

  u32 defs_start = pkgapi->defs.len;
  nodearray_t defs = {0};

  // add unit-local declarations & definitions to topologically-sorted array "defs"
  nodeflag_t visibility = 0;
  for (u32 i = 0; i < children.len; i++) {
    node_t* n = children.v[i];
    if (!ast_toposort_visit_def(&defs, g->ma, visibility, n))
      goto end; // OOM
  }
  //dlog_defs(g, defs.v, defs.len, "unit> ");

  // reset source tracking
  g->srcfileid = 0;

  // Assign mangledname.
  // We must do this for all definitions before calling gen_def as the mangledname
  // of a node may be needed in the body of another node preceeding it.
  for (u32 i = 0; i < defs.len; i++) {
    node_t* n = defs.v[i];
    if (nodearray_indexof(&pkgapi->defs, n) == -1) {
      assign_mangledname(g, n);
    } else if (n->kind != EXPR_FUN) {
      // erase node with already-generted code
      defs.v[i] = NULL;
    }
  }

  // generate definitions
  for (u32 i = 0; i < defs.len; i++) {
    node_t* n = defs.v[i];
    if (n)
      gen_def(g, n, /*is_impl*/true);
  }

end:
  pkgapi->defs.len = defs_start;
}


err_t cgen_unit_impl(cgen_t* g, unit_t* u, cgen_pkgapi_t* pkgapi) {
  cgen_reset(g);

  if (pkgapi) {
    if (!map_update_replace_ptr(&g->typedefmap, g->ma, &pkgapi->pkg_typedefs))
      return ErrNoMem;
  }

  PRINT("#include <coprelude.h>\n");

  gen_imports(g, u);

  // Include pre-generated package API.
  // This data is usually a copy of g->outbuf after callint cgen_pkg_api
  if (pkgapi && pkgapi->pkg_header.len > 0) {
    PRINT("\n// ------ begin package api ------\n");
    buf_append(&g->outbuf, pkgapi->pkg_header.p, pkgapi->pkg_header.len);
    if ( ((u8*)pkgapi->pkg_header.p)[pkgapi->pkg_header.len-1] != '\n' )
      CHAR('\n');
    PRINT("// ------ end package api ------\n");
  }

  usize headstart = g->outbuf.len;

  gen_unit(g, u, pkgapi);

  if (g->mainfun && (g->flags & CGEN_EXE))
    gen_main(g);

  return finalize(g, headstart);
}


void cgen_pkgapi_dispose(cgen_t* g, cgen_pkgapi_t* pkgapi) {
  if (g->ma == NULL)
    return;
  str_free(pkgapi->pkg_header);
  map_dispose(&pkgapi->pkg_typedefs, g->ma);
  nodearray_dispose(&pkgapi->defs, g->ma);
}


static usize count_linefeeds(const char* p, usize len) {
  u32 n = 0;
  for (const char* end = p + len; p < end; p++)
    n += (u32)(*p == '\n');
  return n;
}


static void cgen_pkgapi_maybe_add_c_header(cgen_t* g) {
  #define C_PUB_API_HEADER_FILE "pub-api.co.h"

  str_t hfile = path_join(g->pkg->dir.p, C_PUB_API_HEADER_FILE);

  const void* data;
  struct stat st;
  err_t err = mmap_file_ro(hfile.p, &data, &st);
  if (err) {
    if (err != ErrNotFound)
      vlog("ignoring %s (%s)", hfile.p, err_str(err));
    goto end;
  }
  if (st.st_size == 0 || (st.st_size == 1 && *(const u8*)data == '\n')) {
    vlog("ignoring %s (empty)", hfile.p);
    goto close;
  }

  vlog("embedding \"%s\" in " PKG_APIHFILE_NAME, hfile.p);

  // set source file and line info
  PRINT("\n// ---- begin " C_PUB_API_HEADER_FILE " ----\n"
        "#line 1 \"");
  buf_appendrepr(&g->outbuf, hfile.p, hfile.len);
  PRINT("\"\n");

  // check for relative includes, which are not supported since we are embedding
  #define LOCAL_CPP_INCLUDE "#include \""
  isize ipos = string_indexofstr(
    data, st.st_size, LOCAL_CPP_INCLUDE, strlen(LOCAL_CPP_INCLUDE));
  if UNLIKELY(ipos != -1) {
    srcfile_t sf = { .name = hfile, .data = data, .size = st.st_size };
    origin_t origin = { .file = &sf, .line = count_linefeeds(data, ipos) + 1 };
    report_diag(g->compiler, origin, DIAG_ERR,
      "package-local include not supported in " C_PUB_API_HEADER_FILE);
    seterr(g, ErrCanceled);
  }

  // copy contents of hfile to outbuf
  buf_append(&g->outbuf, data, st.st_size);
  if (g->outbuf.len == 0 || g->outbuf.chars[g->outbuf.len-1] != '\n')
    buf_push(&g->outbuf, '\n');
  PRINT("// ---- end " C_PUB_API_HEADER_FILE " ----\n");

  // restore source file and line info
  u32 lineno = count_linefeeds(g->outbuf.chars, g->outbuf.len) + 2;
  PRINTF("#line %u \"", lineno);
  hfile.len = 0; // reuse hfile str_t for apihfile
  if (!pkg_buildfile(g->pkg, g->compiler, &hfile, PKG_APIHFILE_NAME))
    seterr(g, ErrNoMem);
  buf_appendrepr(&g->outbuf, hfile.p, hfile.len);
  PRINT("\"\n");

close:
  mmap_unmap(data, st.st_size);
end:
  str_free(hfile);
}


static void add_pkg_defs(
  cgen_t* g, unit_t** unitv, u32 unitc, nodearray_t* defs, nodeflag_t visibility)
{
  // topologically sort type & function definitions
  for (u32 i = 0; i < unitc; i++) {
    nodearray_t children = unitv[i]->children;
    for (u32 i = 0; i < children.len; i++) {
      node_t* n = children.v[i];
      if ((n->flags & visibility) == 0)
        continue;
      if (!ast_toposort_visit_def(defs, g->ma, NF_VIS_PKG | NF_VIS_PUB, n)) {
        nodearray_dispose(defs, g->ma);
        return;
      }
    }
  }
}


static void gen_decls(cgen_t* g, node_t** defv, u32 defc) {
  g->srcfileid = 0;
  for (u32 i = 0; i < defc; i++)
    assign_mangledname(g, defv[i]);
  for (u32 i = 0; i < defc; i++)
    gen_def(g, defv[i], /*is_impl*/false);
}


err_t cgen_pkgapi(cgen_t* g, unit_t** unitv, u32 unitc, cgen_pkgapi_t* pkgapi) {
  memset(pkgapi, 0, sizeof(*pkgapi));
  cgen_reset(g);

  // we assume no AST nodes have been MARK1'd, since we rely on that for toposort
  #ifdef DEBUG
  for (u32 i = 0; i < unitc; i++) {
    nodearray_t children = unitv[i]->children;
    for (u32 i = 0; i < children.len; i++) {
      const node_t* n = children.v[i];
      assertf((n->flags & NF_MARK1) == 0, "%s#%p", nodekind_name(n->kind), n);
    }
  }
  #endif

  PRINT("// package "), PRINTN(g->pkg->path.p, g->pkg->path.len), CHAR('\n');
  PRINT("#pragma once\n");
  usize headstart = g->outbuf.len;

  cgen_pkgapi_maybe_add_c_header(g);


  // -- public --

  usize section_start = g->outbuf.len;

  add_pkg_defs(g, unitv, unitc, &pkgapi->defs, NF_VIS_PUB);
  //dlog_defs(g, pkgapi->defs.v, pkgapi->defs.len, "pub> ");
  gen_decls(g, pkgapi->defs.v, pkgapi->defs.len);

  if (section_start < g->outbuf.len)
    CHAR('\n'); g->lineno++;

  usize pub_header_len = g->outbuf.len;


  // -- package --

  PRINT("\n// internal API:"); g->lineno++;
  section_start = g->outbuf.len;

  u32 defs_start = pkgapi->defs.len;
  add_pkg_defs(g, unitv, unitc, &pkgapi->defs, NF_VIS_PKG);
  if (pkgapi->defs.len > 0) {
    //dlog_defs(g, pkgapi->defs.v+defs_start, pkgapi->defs.len-defs_start, "pkg> ");
    gen_decls(g, pkgapi->defs.v+defs_start, pkgapi->defs.len-defs_start);
  }

  if (section_start == g->outbuf.len) {
    // undo
    g->outbuf.len = pub_header_len;
  }


  if (g->err || ( g->err = finalize(g, headstart) ))
    goto end;
  pub_header_len += g->headbuf.len;

  // Create result map of package-level type definitions by moving g->typedefmap
  // to results and creating a new small empty map for g->typedefmap.
  map_t newtypedefmap;
  if (!map_init(&newtypedefmap, g->ma, 8)) {
    g->err = ErrNoMem;
  } else {
    pkgapi->pkg_typedefs = g->typedefmap;
    g->typedefmap = newtypedefmap;
  }

  // Public API header is the first pub_header_len bytes of outbuf
  pkgapi->pub_header = buf_slice(g->outbuf, 0, pub_header_len);

  // Package-internal API is outbuf[headstart:]
  // We must copy this since it will be reused with cgen_unit_impl,
  // which in turn recycles outbuf.
  pkgapi->pkg_header = str_makelen(g->outbuf.p + headstart, g->outbuf.len - headstart);
  if (pkgapi->pkg_header.cap == 0 || g->outbuf.oom)
    seterr(g, ErrNoMem);

end:
  if (g->err)
    cgen_pkgapi_dispose(g, pkgapi);
  return g->err;
}
