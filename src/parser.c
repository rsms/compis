#include "c0lib.h"
#include "compiler.h"
#include "abuf.h"

#include <stdlib.h>

#define LOG_PRATT(fmt, args...) log("parse> " fmt, ##args)
#if !defined(LOG_PRATT) || !defined(DEBUG)
  #undef LOG_PRATT
  #define LOG_PRATT(args...) ((void)0)
#else
  #define LOG_PRATT_ENABLED
#endif

typedef enum {
  PREC_COMMA,         // ,
  PREC_ASSIGN,        // =  +=  -=  |=  (et al ...)
  PREC_LOGICAL_OR,    // ||
  PREC_LOGICAL_AND,   // &&
  PREC_BITWISE_OR,    // |
  PREC_BITWISE_XOR,   // ^
  PREC_BITWISE_AND,   // &
  PREC_EQUAL,         // ==  !=
  PREC_COMPARE,       // <  <=  >  >=
  PREC_SHIFT,         // <<  >>
  PREC_ADD,           // +  -
  PREC_MUL,           // *  /  %
  PREC_UNARY_PREFIX,  // ++  --  +  -  !  ~  *  &
  PREC_UNARY_POSTFIX, // ++  --  ()  []
  PREC_MEMBER,        // .

  PREC_LOWEST = PREC_COMMA,
} precedence_t;

//#define PPARAMS parser_t* p, precedence_t prec
#define PARGS   p, prec

typedef node_t*(*prefix_expr_parselet_t)(parser_t* p, precedence_t prec);
typedef node_t*(*infix_expr_parselet_t)(parser_t* p, precedence_t prec, node_t* left);

typedef type_t*(*prefix_type_parselet_t)(parser_t* p, precedence_t prec);
typedef type_t*(*infix_type_parselet_t)(parser_t* p, precedence_t prec, type_t* left);

typedef struct {
  prefix_expr_parselet_t nullable prefix;
  infix_expr_parselet_t  nullable infix;
  precedence_t                    prec;
} expr_parselet_t;

typedef struct {
  prefix_type_parselet_t nullable prefix;
  infix_type_parselet_t  nullable infix;
  precedence_t                    prec;
} type_parselet_t;

// parselet table (defined towards end of file)
static const expr_parselet_t expr_parsetab[TOK_COUNT];
static const type_parselet_t type_parsetab[TOK_COUNT];

// keyword table
static const struct { const char* s; tok_t t; } keywordtab[] = {
  #define _(NAME, ...)
  #define KEYWORD(str, NAME) {str, NAME},
  #include "tokens.h"
  #undef _
  #undef KEYWORD
};

// last_resort_node is returned by mknode when memory allocation fails
node_t* last_resort_node = &(node_t){ .kind = NODE_BAD };

// last_resort_cstr is returned by mkcstr when memory allocation fails
static char last_resort_cstr[1] = {0};
static char underscore_cstr[2] = {"_"};


static u32 u64log10(u64 u) {
  // U64_MAX 18446744073709551615
  u32 w = 20;
  u64 x = 10000000000000000000llu;
  while (w > 1) {
    if (u >= x)
      break;
    x /= 10;
    w--;
  }
  return w;
}


inline static tok_t currtok(parser_t* p) {
  return p->scanner.tok.t;
}


inline static srcloc_t currloc(parser_t* p) {
  return p->scanner.tok.loc;
}


#ifdef LOG_PRATT_ENABLED
  static void log_pratt(parser_t* p, const char* msg) {
    log("parse> %s:%u:%u\t%-12s %s",
      p->scanner.tok.loc.input->name,
      p->scanner.tok.loc.line,
      p->scanner.tok.loc.col,
      tok_name(currtok(p)),
      msg);
  }
  static void log_pratt_infix(
    parser_t* p, const char* class,
    const void* nullable parselet_infix, precedence_t parselet_prec,
    precedence_t ctx_prec)
  {
    char buf[128];
    abuf_t a = abuf_make(buf, sizeof(buf));
    abuf_fmt(&a, "infix %s ", class);
    if (parselet_infix && parselet_prec >= ctx_prec) {
      abuf_str(&a, "match");
    } else if (parselet_infix) {
      abuf_fmt(&a, "(skip; prec(%d) < ctx_prec(%d))", parselet_prec, ctx_prec);
    } else {
      abuf_str(&a, "(no match)");
    }
    abuf_terminate(&a);
    return log_pratt(p, buf);
  }
#else
  #define log_pratt_infix(...) ((void)0)
  #define log_pratt(...) ((void)0)
#endif


static void maybe_keyword(parser_t* p) {
  // binary search for matching keyword & convert currtok to keyword
  usize low = 0, high = countof(keywordtab), mid;
  int cmp;
  slice_t lit = scanner_lit(&p->scanner);

  while (low < high) {
    mid = (low + high) / 2;
    cmp = strncmp(lit.chars, keywordtab[mid].s, lit.len);
    //dlog("maybe_keyword %.*s <> %s = %d",
    //  (int)lit.len, lit.chars, keywordtab[mid].s, cmp);
    if (cmp == 0) {
      p->scanner.tok.t = keywordtab[mid].t;
      break;
    }
    if (cmp < 0) {
      high = mid;
    } else {
      low = mid + 1;
    }
  }
}


static void next(parser_t* p) {
  scanner_next(&p->scanner);
  if (currtok(p) == TID)
    maybe_keyword(p);
}


// fastforward advances the scanner until one of the tokens in stoplist is encountered.
// The stoplist token encountered is consumed.
// stoplist should be NULL-terminated.
static void fastforward(parser_t* p, const tok_t* stoplist) {
  while (currtok(p) != TEOF) {
    const tok_t* tp = stoplist;
    while (*tp) {
      if (*tp++ == currtok(p))
        goto end;
    }
    next(p);
  }
end:
  next(p);
}

static void fastforward_semi(parser_t* p) {
  fastforward(p, (const tok_t[]){ TSEMI, 0 });
}


// node_srcrange computes the source range for an AST
srcrange_t node_srcrange(node_t* n) {
  srcrange_t r = { .start = n->loc, .focus = n->loc };
  switch (n->kind) {
    case EXPR_INTLIT:
      r.end.line = r.focus.line;
      r.end.col = r.focus.col + u64log10(n->intval);
      break;
    default: if (node_has_strval(n)) {
      r.end.line = r.focus.line;
      r.end.col = r.focus.col + strlen(n->strval);
    }
  }
  return r;
}


ATTR_FORMAT(printf,3,4)
static void error(parser_t* p, const node_t* nullable n, const char* fmt, ...) {
  srcrange_t range = { .focus = currloc(p), };
  if (n)
    dlog("TODO node_srcrange(n)");
  va_list ap;
  va_start(ap, fmt);
  report_errorv(p->scanner.compiler, range, fmt, ap);
  va_end(ap);
}


static void unexpected(parser_t* p, const char* errmsg) {
  char descr[64];
  tok_t tok = currtok(p);
  tok_descrs(descr, sizeof(descr), tok, scanner_lit(&p->scanner));
  int msglen = (int)strlen(errmsg) + (*errmsg != 0);
  error(p, NULL, "unexpected %s%*s", descr, msglen, errmsg);
}


static void expect_fail(parser_t* p, tok_t expecttok, const char* errmsg) {
  char want[64], got[64];
  tok_descrs(want, sizeof(want), expecttok, (slice_t){0});
  tok_descrs(got, sizeof(got), currtok(p), scanner_lit(&p->scanner));
  int msglen = (int)strlen(errmsg) + (*errmsg != 0);
  error(p, NULL, "expected %s%*s, got %s", want, msglen, errmsg, got);
}


static bool expect_token(parser_t* p, tok_t expecttok, const char* errmsg) {
  bool ok = currtok(p) == expecttok;
  if UNLIKELY(!ok)
    expect_fail(p, expecttok, errmsg);
  return ok;
}


static bool expect(parser_t* p, tok_t expecttok, const char* errmsg) {
  bool ok = expect_token(p, expecttok, errmsg);
  next(p);
  return ok;
}


static void out_of_mem(parser_t* p) {
  error(p, NULL, "out of memory");
  // end scanner, making sure we don't keep going
  p->scanner.inp = p->scanner.inend;
}


static char* mkcstr(parser_t* p, slice_t src) {
  char* s = mem_strdup(p->ast_ma, src, 0);
  if UNLIKELY(s == NULL) {
    out_of_mem(p);
    s = last_resort_cstr;
  }
  return s;
}


static node_t* mknode(parser_t* p, nodekind_t kind) {
  node_t* n = mem_alloct(p->ast_ma, node_t);
  if UNLIKELY(n == NULL)
    return out_of_mem(p), last_resort_node;
  n->kind = kind;
  n->loc = currloc(p);
  return n;
}


static type_t* mktype(parser_t* p, typekind_t kind) {
  assertf(
    kind != TYPE_VOID &&
    kind != TYPE_BOOL &&
    kind != TYPE_I8 &&
    kind != TYPE_I16 &&
    kind != TYPE_I32 &&
    kind != TYPE_I64 &&
    kind != TYPE_I8 &&
    kind != TYPE_I16 &&
    kind != TYPE_I32 &&
    kind != TYPE_I64 &&
    kind != TYPE_F32 &&
    kind != TYPE_F64 ,
    "use type_ constant instead"
  );
  type_t* t = mem_alloct(p->ast_ma, type_t);
  if UNLIKELY(t == NULL)
    return out_of_mem(p), type_void;
  t->kind = kind;
  t->loc = currloc(p);
  return t;
}


static node_t* mkbad(parser_t* p) {
  return mknode(p, NODE_BAD);
}


static void push_child(parser_t* p, node_t* restrict parent, node_t* restrict child) {
  assert(parent != child);
  if UNLIKELY(!nodearray_push(&parent->children, p->ast_ma, child))
    out_of_mem(p);
}


static node_t* expr(parser_t* p, precedence_t prec) {
  tok_t tok = currtok(p);
  const expr_parselet_t* parselet = &expr_parsetab[tok];
  log_pratt(p, "prefix expr");
  if UNLIKELY(!parselet->prefix) {
    unexpected(p, "where an expression is expected");
    fastforward_semi(p);
    return mkbad(p);
  }
  node_t* n = parselet->prefix(PARGS);
  for (;;) {
    tok = currtok(p);
    parselet = &expr_parsetab[tok];
    log_pratt_infix(p, "expr", parselet->infix, parselet->prec, prec);
    if (parselet->infix == NULL || parselet->prec < prec)
      return n;
    n = parselet->infix(PARGS, n);
  }
}


static type_t* type(parser_t* p, precedence_t prec) {
  tok_t tok = currtok(p);
  const type_parselet_t* parselet = &type_parsetab[tok];
  log_pratt(p, "prefix type");
  if UNLIKELY(!parselet->prefix) {
    unexpected(p, "where a type is expected");
    next(p);
    return type_void;
  }
  type_t* t = parselet->prefix(PARGS);
  for (;;) {
    tok = currtok(p);
    parselet = &type_parsetab[tok];
    log_pratt_infix(p, "type", parselet->infix, parselet->prec, prec);
    if (parselet->infix == NULL || parselet->prec < prec)
      return t;
    t = parselet->infix(PARGS, t);
  }
}


static type_t* named_type(parser_t* p, slice_t name, srcloc_t origin) {
  // TODO: proper scope lookup
  if (strncmp("int", name.chars, name.len) == 0)
    return mktype(p, TYPE_INT);
  report_error(p->scanner.compiler, (srcrange_t){ .focus = origin },
    "unknown type \"%.*s\"", (int)name.len, name.chars);
  return type_void;
}


static type_t* prefix_type_id(parser_t* p, precedence_t prec) {
  slice_t lit = scanner_lit(&p->scanner);
  type_t* t = named_type(p, lit, currloc(p));
  next(p);
  return t;
}


static node_t* prefix_id(parser_t* p, precedence_t prec) {
  node_t* n = mknode(p, EXPR_ID);
  slice_t lit = scanner_lit(&p->scanner);
  n->strval = mkcstr(p, lit);
  next(p);
  return n;
}


static node_t* id(parser_t* p, const char* helpmsg) {
  if UNLIKELY(currtok(p) != TID)
    unexpected(p, helpmsg);
  return prefix_id(p, PREC_LOWEST);
}


static node_t* prefix_intlit(parser_t* p, precedence_t prec) {
  node_t* n = mknode(p, EXPR_INTLIT);
  n->intval = p->scanner.litint;
  next(p);
  return n;
}


static node_t* prefix_op(parser_t* p, precedence_t prec) {
  node_t* n = mknode(p, EXPR_PREFIXOP);
  n->op1.op = currtok(p);
  next(p);
  n->op1.expr = expr(p, prec);
  return n;
}


static node_t* postfix_op(parser_t* p, precedence_t prec, node_t* left) {
  node_t* n = mknode(p, EXPR_POSTFIXOP);
  n->op1.op = currtok(p);
  next(p);
  n->op1.expr = expr(p, prec);
  return n;
}


static node_t* infix_op(parser_t* p, precedence_t prec, node_t* left) {
  node_t* n = mknode(p, EXPR_INFIXOP);
  n->op2.op = currtok(p);
  next(p);
  n->op2.left = left;
  n->op2.right = expr(p, prec);
  return n;
}


static node_t* postfix_paren(parser_t* p, precedence_t prec, node_t* left) {
  node_t* n = mknode(p, EXPR_POSTFIXOP);
  next(p);
  panic("TODO");
  return n;
}


static node_t* postfix_brack(parser_t* p, precedence_t prec, node_t* left) {
  node_t* n = mknode(p, EXPR_POSTFIXOP);
  next(p);
  panic("TODO");
  return n;
}


static node_t* postfix_member(parser_t* p, precedence_t prec, node_t* left) {
  node_t* n = mknode(p, EXPR_POSTFIXOP);
  next(p);
  panic("TODO");
  return n;
}


static node_t* prefix_block(parser_t* p, precedence_t prec) {
  tok_t endtok = currtok(p) == TINDENT ? TDEDENT : TRBRACE;
  node_t* n = mknode(p, EXPR_BLOCK);
  next(p);
  while (currtok(p) != endtok && currtok(p) != TEOF) {
    push_child(p, n, expr(p, PREC_LOWEST));
    if (currtok(p) != TSEMI)
      break;
    next(p); // consume ";"
  }
  expect(p, endtok, "to end block");
  expect(p, TSEMI, "after block");
  return n;
}


static void params(parser_t* p, fieldarray_t* params) {
  // params = "(" param (sep param)* sep? ")"
  // param  = Id Type? | Type
  // sep    = "," | ";"
  //
  // e.g.  (T)  (x T)  (x, y T)  (T1, T2, T3)

  // true when at least one param has type; e.g. "x T"
  bool isnametype = false;

  // typeq: temporary storage for fields to support "typed groups" of parameters,
  // e.g. "x, y int" -- "x" does not have a type until we parsed "y" and "int", so when
  // we parse "x" we put it in typeq. Also, "x" might be just a type and not a name in
  // the case all args are just types e.g. "T1, T2, T3".
  array_t typeq = {0}; // field_t*[]

  while (currtok(p) != TEOF) {
    field_t* field = fieldarray_alloc(params, p->ast_ma, 1);
    if UNLIKELY(field == NULL)
      return out_of_mem(p);

    if (currtok(p) == TID) {
      // name, eg "x"; could be field name or type. Assume field name for now.
      field->name = mkcstr(p, scanner_lit(&p->scanner));
      field->loc = currloc(p);
      next(p);
      switch (currtok(p)) {
      case TRPAREN:
      case TCOMMA:
      case TSEMI: // just a name, eg "x" in "(x, y)"
        if (!array_push(field_t*, &typeq, p->ast_ma, field))
          return out_of_mem(p);
        break;
      default: // type follows name, eg "int" in "x int"
        field->type = type(p, PREC_LOWEST);
        isnametype = true;
        // cascade type to predecessors
        for (usize i = 0; i < typeq.len; i++)
          array_at(field_t*, &typeq, i)->type = field->type;
        typeq.len = 0;
      }
    } else {
      // definitely a type
      field->name = underscore_cstr;
      if (!field->name)
        return out_of_mem(p);
      field->type = type(p, PREC_LOWEST);
    }
    switch (currtok(p)) {
      case TCOMMA:
      case TSEMI:
        next(p); // consume "," or ";"
        if (currtok(p) == TRPAREN)
          goto finish; // trailing "," or ";"
        break; // continue reading more
      case TRPAREN:
        goto finish;
      default:
        unexpected(p, "expecting ',' ';' or ')'");
        fastforward(p, (const tok_t[]){ TRPAREN, 0 });
        goto finish;
    }
  }
finish:
  if (isnametype) {
    // name-and-type form; e.g. "(x, y T, z Y)".
    // Error if at least one param has type, but last one doesn't, e.g. "(x, y int, z)"
    if (typeq.len > 0)
      error(p, NULL, "expecting type");
    // for (usize i = 0; i < params->len; i++) {
    //   // TODO: defsym(p, params->v[i].name, param)
    // }
  } else {
    // type-only form, e.g. "(T, T, Y)"
    for (usize i = 0; i < params->len; i++) {
      field_t* param = &params->v[i];
      if (param->type)
        continue;
      // make type from id
      param->type = named_type(p, slice_cstr(param->name), param->loc);
      param->name = underscore_cstr;
    }
  }
  array_dispose(field_t*, &typeq, p->ast_ma);
}


// fundef = "fun" name "(" params? ")" result ( ";" | "{" body "}")
// result = params
// body   = (stmt ";")*
static node_t* prefix_fun(parser_t* p, precedence_t prec) {
  node_t* n = mknode(p, EXPR_FUN);
  next(p);
  n->fun.name = id(p, "after fun");
  expect(p, TLPAREN, "for parameters");
  if (currtok(p) != TRPAREN)
    params(p, &n->fun.params);
  expect(p, TRPAREN, "to end parameters");
  n->fun.result_type = type(p, prec);
  if (currtok(p) == TSEMI) {
    next(p);
  } else {
    n->fun.body = expr(p, PREC_LOWEST);
  }
  return n;
}


static const expr_parselet_t expr_parsetab[TOK_COUNT] = {
  // infix ops (in order of precedence from weakest to strongest)
  [TCOMMA]     = {NULL, infix_op, PREC_COMMA},
  [TASSIGN]    = {NULL, infix_op, PREC_ASSIGN}, // =
  [TMULASSIGN] = {NULL, infix_op, PREC_ASSIGN}, // *=
  [TDIVASSIGN] = {NULL, infix_op, PREC_ASSIGN}, // /=
  [TMODASSIGN] = {NULL, infix_op, PREC_ASSIGN}, // %=
  [TADDASSIGN] = {NULL, infix_op, PREC_ASSIGN}, // +=
  [TSUBASSIGN] = {NULL, infix_op, PREC_ASSIGN}, // -=
  [TSHLASSIGN] = {NULL, infix_op, PREC_ASSIGN}, // <<=
  [TSHRASSIGN] = {NULL, infix_op, PREC_ASSIGN}, // >>=
  [TANDASSIGN] = {NULL, infix_op, PREC_ASSIGN}, // &=
  [TXORASSIGN] = {NULL, infix_op, PREC_ASSIGN}, // ^=
  [TORASSIGN]  = {NULL, infix_op, PREC_ASSIGN}, // |=
  [TOROR]      = {NULL, infix_op, PREC_LOGICAL_OR}, // ||
  [TANDAND]    = {NULL, infix_op, PREC_LOGICAL_AND}, // &&
  [TOR]        = {NULL, infix_op, PREC_BITWISE_OR}, // |
  [TXOR]       = {NULL, infix_op, PREC_BITWISE_XOR}, // ^
  [TAND]       = {prefix_op, infix_op, PREC_BITWISE_AND}, // &
  [TEQ]        = {NULL, infix_op, PREC_EQUAL}, // ==
  [TNEQ]       = {NULL, infix_op, PREC_EQUAL}, // !=
  [TLT]        = {NULL, infix_op, PREC_COMPARE},   // <
  [TGT]        = {NULL, infix_op, PREC_COMPARE},   // >
  [TLTEQ]      = {NULL, infix_op, PREC_COMPARE}, // <=
  [TGTEQ]      = {NULL, infix_op, PREC_COMPARE}, // >=
  [TSHL]       = {NULL, infix_op, PREC_SHIFT}, // >>
  [TSHR]       = {NULL, infix_op, PREC_SHIFT}, // <<
  [TPLUS]      = {prefix_op, infix_op, PREC_ADD}, // +
  [TMINUS]     = {prefix_op, infix_op, PREC_ADD}, // -
  [TSTAR]      = {prefix_op, infix_op, PREC_MUL}, // *
  [TSLASH]     = {NULL, infix_op, PREC_MUL}, // /
  [TPERCENT]   = {NULL, infix_op, PREC_MUL}, // %

  // prefix and postfix ops (in addition to the ones above)
  [TPLUSPLUS]   = {prefix_op, postfix_op, PREC_UNARY_PREFIX}, // ++
  [TMINUSMINUS] = {prefix_op, postfix_op, PREC_UNARY_PREFIX}, // --
  [TNOT]        = {prefix_op, NULL, PREC_UNARY_PREFIX}, // !
  [TTILDE]      = {prefix_op, NULL, PREC_UNARY_PREFIX}, // ~

  // postfix ops
  [TLPAREN] = {NULL, postfix_paren, PREC_UNARY_POSTFIX}, // (
  [TLBRACK] = {NULL, postfix_brack, PREC_UNARY_POSTFIX}, // [

  // member ops
  [TDOT] = {NULL, postfix_member, PREC_MEMBER}, // .

  // keywords & identifiers
  [TID]     = {prefix_id, NULL, PREC_MEMBER},
  [TFUN]    = {prefix_fun, NULL, PREC_MEMBER},
  [TINTLIT] = {prefix_intlit, NULL, PREC_MEMBER},

  // block
  [TINDENT] = {prefix_block, NULL, PREC_MEMBER},
  [TLBRACE] = {prefix_block, NULL, PREC_MEMBER},
};


static const type_parselet_t type_parsetab[TOK_COUNT] = {
  [TID] = {prefix_type_id, NULL, PREC_MEMBER},
};


node_t* parser_parse(parser_t* p, memalloc_t ast_ma, input_t* input) {
  p->ast_ma = ast_ma;
  scanner_set_input(&p->scanner, input);
  node_t* unit = mknode(p, NODE_UNIT);
  next(p);

  // parse unit-level declarations
  while (currtok(p) != TEOF) {
    switch (currtok(p)) {
    case TFUN:
      push_child(p, unit, prefix_fun(p, PREC_LOWEST));
      break;
    default:
      unexpected(p, "");
      fastforward_semi(p);
    }
    if (currtok(p) == TSEMI) {
      unexpected(p, "at top-level");
      next(p);
    }
  }

  // double strtod(const char *restrict s, char **restrict p)
  return unit;
}


void parser_init(parser_t* p, compiler_t* c) {
  memset(p, 0, sizeof(*p));
  scanner_init(&p->scanner, c);
  // keywordtab must be sorted
  #if DEBUG
    for (usize i = 1; i < countof(keywordtab); i++)
      assertf(strcmp(keywordtab[i-1].s, keywordtab[i].s) < 0,
        "keywordtab out of order (%s)", keywordtab[i].s);
  #endif
}


void parser_dispose(parser_t* p) {
  scanner_dispose(&p->scanner);
}
