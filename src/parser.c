#include "c0lib.h"
#include "compiler.h"
#include "abuf.h"

#include <stdlib.h>

#define LOG_PRATT(fmt, args...) dlog("[pratt] " fmt, ##args)
#ifndef LOG_PRATT
  #define LOG_PRATT(args...) ((void)0)
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

typedef node_t*(*prefixparselet_t)(parser_t* p, precedence_t prec);
typedef node_t*(*infixparselet_t)(parser_t* p, precedence_t prec, node_t* left);

typedef struct {
  prefixparselet_t nullable prefix;
  infixparselet_t  nullable infix;
  precedence_t              prec;
} parselet_t;


// parselet table (defined towards end of file)
static const parselet_t expr_parsetab[TOK_COUNT];

// keyword table
static const struct { const char* s; tok_t t; } keywordtab[] = {
  #define _(NAME, ...)
  #define KEYWORD(str, NAME) {str, NAME},
  #include "tokens.h"
  #undef _
  #undef KEYWORD
};

// last_resort_node is returned by mknode when memory allocation fails
node_t last_resort_node = { .kind = NBAD };

// last_resort_cstr is returned by mkcstr when memory allocation fails
static char last_resort_cstr[1] = {0};


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


inline static tok_t currtok(parser_t* p) {
  return p->scanner.tok.t;
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
  const tok_t stoplist[] = { TSEMI, 0 };
  fastforward(p, stoplist);
}


// node_srcrange computes the source range for an AST
srcrange_t node_srcrange(node_t* n) {
  srcrange_t r = { .start = n->loc, .focus = n->loc };
  switch (n->kind) {
    case NINTLIT:
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
  srcrange_t range = {
    .focus = p->scanner.tok.loc,
  };
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


static char* mkcstr(parser_t* p, slice_t src) {
  char* s = mem_strdup(p->ast_ma, src, 0);
  if UNLIKELY(s == NULL) {
    // end scanner, making sure we don't keep going
    p->scanner.inp = p->scanner.inend;
    s = last_resort_cstr;
  }
  return s;
}


static node_t* mknode(parser_t* p, nodekind_t kind) {
  node_t* n = mem_alloct(p->ast_ma, node_t);
  if UNLIKELY(n == NULL) {
    error(p, NULL, "out of memory");
    // end scanner, making sure we don't keep going
    p->scanner.inp = p->scanner.inend;
    return &last_resort_node;
  }
  n->kind = kind;
  n->loc = p->scanner.tok.loc;
  return n;
}


static node_t* mkbad(parser_t* p) { return mknode(p, NBAD); }


// returns child
static node_t* addchild(node_t* restrict parent, node_t* restrict child) {
  assert(parent != child);
  if (parent->children.tail) {
    parent->children.tail->next = child;
  } else {
    parent->children.head = child;
  }
  parent->children.tail = child;
  assert(child->next == NULL);
  return child;
}


// static node_t* setchildren1(node_t* restrict parent, node_t* restrict child) {
//   assert(parent != child);
//   parent->children.head = child;
//   parent->children.tail = child;
//   assert(child->next == NULL);
//   return parent;
// }


// static node_t* setchildren2(
//   node_t* restrict parent,
//   node_t* restrict child1,
//   node_t* restrict nullable child2)
// {
//   assert(parent != child1);
//   assert(parent != child2);
//   parent->children.head = child1;
//   parent->children.tail = child2 ? child2 : child1;
//   child1->next = child2;
//   assert(child2 == NULL || child2->next == NULL);
//   return parent;
// }


static node_t* expr(parser_t* p, precedence_t prec) {
  tok_t tok = currtok(p);
  const parselet_t* parselet = &expr_parsetab[tok];

  if UNLIKELY(!parselet->prefix) {
    LOG_PRATT("PREFIX %s not found", tok_name(tok));
    unexpected(p, "");
    fastforward_semi(p);
    return mkbad(p);
  }

  LOG_PRATT("PREFIX %s", tok_name(tok));

  // save state for assertion after prefix() call
  UNUSED const void* p1 = p->scanner.inp;
  UNUSED bool insertsemi = p->scanner.insertsemi;

  node_t* n = parselet->prefix(PARGS);

  assertf(
    insertsemi != p->scanner.insertsemi ||
    (uintptr)p1 < (uintptr)p->scanner.inp,
    "parselet did not advance scanner");

  // call any infix parselets
  for (;;) {
    tok = currtok(p);
    parselet = &expr_parsetab[tok];
    if (parselet->infix == NULL || parselet->prec < prec) {
      if (parselet->infix) {
        LOG_PRATT("INFIX %s skip; expr_parsetab[%u].prec < caller_prec (%d < %d)",
          tok_name(tok), tok, parselet->prec, prec);
      } else if (tok != TSEMI) {
        LOG_PRATT("INFIX %s not found", tok_name(tok));
      }
      return n;
    }
    LOG_PRATT("INFIX %s", tok_name(tok));
    n = parselet->infix(PARGS, n);
  }

  return n;
}


static node_t* mkid(parser_t* p) {
  node_t* n = mknode(p, NID);
  n->strval = mkcstr(p, scanner_lit(&p->scanner));
  return n;
}


static node_t* id(parser_t* p, const char* helpmsg) {
  node_t* n = mkid(p);
  expect(p, TID, helpmsg);
  return n;
}


static node_t* prefix_id(parser_t* p, precedence_t prec) {
  node_t* n = mkid(p);
  next(p);
  return n;
}


static node_t* prefix_intlit(parser_t* p, precedence_t prec) {
  node_t* n = mknode(p, NINTLIT);
  n->intval = p->scanner.litint;
  next(p);
  return n;
}


static node_t* prefix_op(parser_t* p, precedence_t prec) {
  node_t* n = mknode(p, NPREFIXOP);
  next(p);
  addchild(n, expr(p, prec));
  return n;
}


static node_t* postfix_op(parser_t* p, precedence_t prec, node_t* left) {
  node_t* n = mknode(p, NSUFFIXOP);
  next(p);
  addchild(n, left);
  return n;
}


static node_t* infix_op(parser_t* p, precedence_t prec, node_t* left) {
  node_t* n = mknode(p, NINFIXOP);
  next(p);
  addchild(n, left);
  addchild(n, expr(p, prec));
  return n;
}


static node_t* postfix_paren(parser_t* p, precedence_t prec, node_t* left) {
  node_t* n = mknode(p, NSUFFIXOP);
  next(p);
  dlog("TODO %s", __FUNCTION__);
  return n;
}


static node_t* postfix_brack(parser_t* p, precedence_t prec, node_t* left) {
  node_t* n = mknode(p, NSUFFIXOP);
  next(p);
  dlog("TODO %s", __FUNCTION__);
  return n;
}


static node_t* postfix_member(parser_t* p, precedence_t prec, node_t* left) {
  node_t* n = mknode(p, NSUFFIXOP);
  next(p);
  dlog("TODO %s", __FUNCTION__);
  return n;
}


static node_t* block(parser_t* p, tok_t endtok) {
  node_t* n = mknode(p, NBLOCK);
  next(p);
  while (currtok(p) != endtok && currtok(p) != TEOF) {
    addchild(n, expr(p, PREC_LOWEST));
    // ends with ";" or endtok
    if (currtok(p) != TSEMI && currtok(p) != endtok) {
      expect_fail(p, TSEMI, "after expression");
      break;
    }
    next(p);
  }
  expect(p, endtok, "to end block");
  return n;
}


static node_t* prefix_indent(parser_t* p, precedence_t prec) {
  return block(p, TDEDENT);
}


// fundef = "fun" name "(" params? ")" result ( ";" | "{" body "}")
// result = params
// body   = (stmt ";")*
static node_t* prefix_fun(parser_t* p, precedence_t prec) {
  node_t* n = mknode(p, NFUN);
  next(p);
  node_t* name = addchild(n, id(p, "after fun"));
  expect(p, TLPAREN, "for parameters");
  // TODO: parameters
  expect(p, TRPAREN, "to end parameters");
  node_t* result = addchild(n, expr(p, PREC_LOWEST));
  switch (currtok(p)) {
  case TSEMI:   next(p); return n; // no body
  case TINDENT: addchild(n, block(p, TDEDENT)); break;
  case TLBRACE: addchild(n, block(p, TRBRACE)); break;
  default:      unexpected(p, "where block is expected");
  }
  return n;
}


static const parselet_t expr_parsetab[TOK_COUNT] = {
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

  // special
  [TINDENT] = {prefix_indent, NULL, PREC_MEMBER},
};


node_t* parser_parse(parser_t* p, memalloc_t ast_ma, input_t* input) {
  p->ast_ma = ast_ma;
  scanner_set_input(&p->scanner, input);
  node_t* unit = mknode(p, NUNIT);
  next(p);

  // parse unit-level declarations
  while (currtok(p) != TEOF) {
    switch (currtok(p)) {
    case TFUN:
      addchild(unit, prefix_fun(p, PREC_LOWEST));
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
