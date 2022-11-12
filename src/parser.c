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

typedef stmt_t*(*prefix_stmt_parselet_t)(parser_t* p, precedence_t prec);
typedef stmt_t*(*infix_stmt_parselet_t)(parser_t* p, precedence_t prec, stmt_t* left);

typedef expr_t*(*prefix_expr_parselet_t)(parser_t* p, precedence_t prec);
typedef expr_t*(*infix_expr_parselet_t)(parser_t* p, precedence_t prec, expr_t* left);

typedef type_t*(*prefix_type_parselet_t)(parser_t* p, precedence_t prec);
typedef type_t*(*infix_type_parselet_t)(parser_t* p, precedence_t prec, type_t* left);

typedef struct {
  prefix_stmt_parselet_t nullable prefix;
  infix_stmt_parselet_t  nullable infix;
  precedence_t                    prec;
} stmt_parselet_t;

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
static const stmt_parselet_t stmt_parsetab[TOK_COUNT];
static const expr_parselet_t expr_parsetab[TOK_COUNT];
static const type_parselet_t type_parsetab[TOK_COUNT];

// last_resort_node is returned by mknode when memory allocation fails
node_t* last_resort_node = &(node_t){ .kind = NODE_BAD };


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


static void next(parser_t* p) {
  scanner_next(&p->scanner);
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
      r.end.col = r.focus.col + u64log10( ((intlitexpr_t*)n)->intval);
      break;
    case EXPR_ID:
      r.end.line = r.focus.line;
      r.end.col = r.focus.col + strlen(((idexpr_t*)n)->sym);
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


static void out_of_mem(parser_t* p) {
  error(p, NULL, "out of memory");
  // end scanner, making sure we don't keep going
  p->scanner.inp = p->scanner.inend;
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


static void enter_scope(parser_t* p) {
  if (!scope_push(&p->scope, p->scanner.compiler->ma))
    out_of_mem(p);
}


static void leave_scope(parser_t* p) {
  scope_pop(&p->scope);
}


static const node_t* nullable lookup_definition(parser_t* p, const void* name) {
  const node_t* n = scope_lookup(&p->scope, name);
  if (n)
    return n;
  void** vp = map_lookup(&p->pkgdefs, name, strlen((const char*)name));
  return vp ? *vp : NULL;
}


static void define(parser_t* p, sym_t name, const node_t* n) {
  dlog("define %s => %s@%p", name, nodekind_name(n->kind), n);
  if (!scope_def(&p->scope, p->scanner.compiler->ma, name, n))
    out_of_mem(p);
}


#define mknode(p, TYPE, kind)  ( (TYPE*)_mknode((p), sizeof(TYPE), (kind)) )

static node_t* _mknode(parser_t* p, usize size, nodekind_t kind) {
  mem_t m = mem_alloc_zeroed(p->ast_ma, size);
  if UNLIKELY(m.p == NULL)
    return out_of_mem(p), last_resort_node;
  node_t* n = m.p;
  n->kind = kind;
  n->loc = currloc(p);
  return n;
}


static type_t* mktype(parser_t* p, nodekind_t kind) {
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
  assertf(kind > TYPE_VOID, "%u is not a type kind", kind);
  type_t* t = mem_alloct(p->ast_ma, type_t);
  if UNLIKELY(t == NULL)
    return out_of_mem(p), type_void;
  t->kind = kind;
  t->loc = currloc(p);
  return t;
}


static void* mkbad(parser_t* p) {
  return mknode(p, node_t, NODE_BAD);
}


static void push_child(parser_t* p, ptrarray_t* children, void* child) {
  if UNLIKELY(!ptrarray_push(children, p->ast_ma, child))
    out_of_mem(p);
}


static stmt_t* stmt(parser_t* p, precedence_t prec) {
  tok_t tok = currtok(p);
  const stmt_parselet_t* parselet = &stmt_parsetab[tok];
  log_pratt(p, "prefix stmt");
  if UNLIKELY(!parselet->prefix) {
    unexpected(p, "where an stmtession is expected");
    fastforward_semi(p);
    return mkbad(p);
  }
  stmt_t* n = parselet->prefix(PARGS);
  for (;;) {
    tok = currtok(p);
    parselet = &stmt_parsetab[tok];
    log_pratt_infix(p, "stmt", parselet->infix, parselet->prec, prec);
    if (parselet->infix == NULL || parselet->prec < prec)
      return n;
    n = parselet->infix(PARGS, n);
  }
}


static expr_t* expr(parser_t* p, precedence_t prec) {
  tok_t tok = currtok(p);
  const expr_parselet_t* parselet = &expr_parsetab[tok];
  log_pratt(p, "prefix expr");
  if UNLIKELY(!parselet->prefix) {
    unexpected(p, "where an expression is expected");
    fastforward_semi(p);
    return mkbad(p);
  }
  expr_t* n = parselet->prefix(PARGS);
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


static type_t* named_type(parser_t* p, sym_t name, srcloc_t origin) {
  // TODO: proper scope lookup
  if (strncmp("int", name, strlen(name)) == 0)
    return mktype(p, TYPE_INT);
  report_error(p->scanner.compiler, (srcrange_t){ .focus = origin },
    "unknown type \"%s\"", name);
  return type_void;
}


static type_t* prefix_type_id(parser_t* p, precedence_t prec) {
  type_t* t = named_type(p, p->scanner.sym, currloc(p));
  next(p);
  return t;
}


static expr_t* prefix_id(parser_t* p, precedence_t prec) {
  idexpr_t* n = mknode(p, idexpr_t, EXPR_ID);
  n->sym = p->scanner.sym;
  next(p);
  return (expr_t*)n;
}


static idexpr_t* id(parser_t* p, const char* helpmsg) {
  if UNLIKELY(currtok(p) != TID)
    unexpected(p, helpmsg);
  return (idexpr_t*)prefix_id(p, PREC_LOWEST);
}


static expr_t* prefix_intlit(parser_t* p, precedence_t prec) {
  intlitexpr_t* n = mknode(p, intlitexpr_t, EXPR_INTLIT);
  n->intval = p->scanner.litint;
  next(p);
  return (expr_t*)n;
}


static expr_t* prefix_op(parser_t* p, precedence_t prec) {
  op1expr_t* n = mknode(p, op1expr_t, EXPR_PREFIXOP);
  n->op = currtok(p);
  next(p);
  n->expr = expr(p, prec);
  return (expr_t*)n;
}


static expr_t* postfix_op(parser_t* p, precedence_t prec, expr_t* left) {
  op1expr_t* n = mknode(p, op1expr_t, EXPR_POSTFIXOP);
  n->op = currtok(p);
  next(p);
  n->expr = expr(p, prec);
  return (expr_t*)n;
}


static expr_t* infix_op(parser_t* p, precedence_t prec, expr_t* left) {
  op2expr_t* n = mknode(p, op2expr_t, EXPR_INFIXOP);
  n->op = currtok(p);
  next(p);
  n->left = left;
  n->right = expr(p, prec);
  return (expr_t*)n;
}


static expr_t* postfix_paren(parser_t* p, precedence_t prec, expr_t* left) {
  op1expr_t* n = mknode(p, op1expr_t, EXPR_POSTFIXOP);
  next(p);
  panic("TODO");
  return (expr_t*)n;
}


static expr_t* postfix_brack(parser_t* p, precedence_t prec, expr_t* left) {
  op1expr_t* n = mknode(p, op1expr_t, EXPR_POSTFIXOP);
  next(p);
  panic("TODO");
  return (expr_t*)n;
}


static expr_t* postfix_member(parser_t* p, precedence_t prec, expr_t* left) {
  op1expr_t* n = mknode(p, op1expr_t, EXPR_POSTFIXOP);
  next(p);
  panic("TODO");
  return (expr_t*)n;
}


static expr_t* prefix_block(parser_t* p, precedence_t prec) {
  tok_t endtok = currtok(p) == TINDENT ? TDEDENT : TRBRACE;
  block_t* n = mknode(p, block_t, EXPR_BLOCK);
  next(p);
  while (currtok(p) != endtok && currtok(p) != TEOF) {
    push_child(p, &n->children, (node_t*)expr(p, PREC_LOWEST));
    if (currtok(p) != TSEMI)
      break;
    next(p); // consume ";"
  }
  expect(p, endtok, "to end block");
  expect(p, TSEMI, "after block");
  return (expr_t*)n;
}


static void params(parser_t* p, ptrarray_t* params) {
  // params = "(" param (sep param)* sep? ")"
  // param  = Id Type? | Type
  // sep    = "," | ";"
  //
  // e.g.  (T)  (x T)  (x, y T)  (T1, T2, T3)

  // true when at least one param has type; e.g. "x T"
  bool isnametype = false;

  // typeq: temporary storage for params to support "typed groups" of parameters,
  // e.g. "x, y int" -- "x" does not have a type until we parsed "y" and "int", so when
  // we parse "x" we put it in typeq. Also, "x" might be just a type and not a name in
  // the case all args are just types e.g. "T1, T2, T3".
  ptrarray_t typeq = {0}; // local_t*[]

  while (currtok(p) != TEOF) {
    local_t* param = mknode(p, local_t, NODE_LOCAL);
    if UNLIKELY(param == NULL)
      return out_of_mem(p);

    if (!ptrarray_push(params, p->ast_ma, param))
      return out_of_mem(p);

    if (currtok(p) == TID) {
      // name, eg "x"; could be parameter name or type. Assume name for now.
      param->name = p->scanner.sym;
      param->loc = currloc(p);
      next(p);
      switch (currtok(p)) {
      case TRPAREN:
      case TCOMMA:
      case TSEMI: // just a name, eg "x" in "(x, y)"
        if (!ptrarray_push(&typeq, p->ast_ma, param))
          return out_of_mem(p);
        break;
      default: // type follows name, eg "int" in "x int"
        param->type = type(p, PREC_LOWEST);
        isnametype = true;
        // cascade type to predecessors
        for (u32 i = 0; i < typeq.len; i++) {
          local_t* prev_param = typeq.v[i];
          prev_param->type = param->type;
        }
        typeq.len = 0;
      }
    } else {
      // definitely a type
      param->name = sym__;
      if (!param->name)
        return out_of_mem(p);
      param->type = type(p, PREC_LOWEST);
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
    for (u32 i = 0; i < params->len; i++)
      define(p, ((local_t*)params->v[i])->name, params->v[i]);
  } else {
    // type-only form, e.g. "(T, T, Y)"
    for (u32 i = 0; i < params->len; i++) {
      local_t* param = (local_t*)params->v[i];
      if (param->type)
        continue;
      // make type from id
      param->type = named_type(p, param->name, param->loc);
      param->name = sym__;
    }
  }
  ptrarray_dispose(&typeq, p->ast_ma);
}


// fundef = "fun" name "(" params? ")" result ( ";" | "{" body "}")
// result = params
// body   = (stmt ";")*
static expr_t* prefix_fun(parser_t* p, precedence_t prec) {
  fun_t* n = mknode(p, fun_t, EXPR_FUN);
  next(p);
  n->name = id(p, "after fun");
  enter_scope(p);
  expect(p, TLPAREN, "for parameters");
  if (currtok(p) != TRPAREN) {
    dlog("n->params: %p (.ptr=%p)", &n->params, n->params.ptr);
    params(p, &n->params);
  }
  expect(p, TRPAREN, "to end parameters");
  n->result_type = type(p, prec);
  if (currtok(p) == TSEMI) {
    next(p);
  } else {
    n->body = expr(p, PREC_LOWEST);
  }
  leave_scope(p);
  return (expr_t*)n;
}


static stmt_t* funstmt(parser_t* p, precedence_t prec) {
  fun_t* n = (fun_t*)prefix_fun(p, prec);
  if (n->kind == EXPR_FUN && !n->name)
    error(p, (node_t*)n, "anonymous function at top level");
  return (stmt_t*)n;
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


static const stmt_parselet_t stmt_parsetab[TOK_COUNT] = {
  [TFUN] = {funstmt, NULL, PREC_MEMBER},
};


unit_t* parser_parse(parser_t* p, memalloc_t ast_ma, input_t* input) {
  p->ast_ma = ast_ma;
  scope_clear(&p->scope);
  scanner_set_input(&p->scanner, input);
  unit_t* unit = mknode(p, unit_t, NODE_UNIT);
  next(p);

  while (currtok(p) != TEOF) {
    stmt_t* n = stmt(p, PREC_LOWEST);
    push_child(p, &unit->children, n);
    if (currtok(p) == TSEMI) {
      unexpected(p, "at top-level");
      next(p);
    }
  }

  // double strtod(const char *restrict s, char **restrict p)
  return unit;
}


static const map_t* universe() {
  static map_t m = {0};
  _Atomic(usize) init = 0;
  if (init++)
    return &m;
  const struct {
    const char* key;
    const void* node;
  } entries[] = {
    {"void", type_void},
    {"bool", type_bool},
    {"int", type_int},
    {"uint", type_uint},
    {"i8", type_i8},
    {"i16", type_i16},
    {"i32", type_i32},
    {"i64", type_i64},
    {"u8", type_u8},
    {"u16", type_u16},
    {"u32", type_u32},
    {"u64", type_u64},
    {"f32", type_f32},
    {"f64", type_f64},
  };
  static void* storage[
    (MEMALLOC_BUMP_OVERHEAD + MAP_STORAGE_X(countof(entries))) / sizeof(void*)] = {0};
  memalloc_t ma = memalloc_bump(storage, sizeof(storage), MEMALLOC_STORAGE_ZEROED);
  safecheckx(map_init(&m, ma, countof(entries)));
  for (usize i = 1; i < countof(entries); i++) {
    void** valp = map_assign(&m, ma, entries[i].key, strlen(entries[i].key));
    assertnotnull(valp);
    *valp = (void*)entries[i].node;
  }
  return &m;
}


bool parser_init(parser_t* p, compiler_t* c) {
  memset(p, 0, sizeof(*p));
  if (!scanner_init(&p->scanner, c))
    return false;
  if (!map_init(&p->pkgdefs, c->ma, 32))
    return false;
  p->pkgdefs.parent = universe();
  return true;
}


void parser_dispose(parser_t* p) {
  map_dispose(&p->pkgdefs, p->scanner.compiler->ma);
  scanner_dispose(&p->scanner);
}
