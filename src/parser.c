#include "c0lib.h"
#include "compiler.h"
#include "abuf.h"

#include <stdlib.h>

#define LOG_PRATT(fmt, args...) dlog("[pratt] " fmt, ##args)
#ifndef LOG_PRATT
  #define LOG_PRATT(args...) ((void)0)
#endif


typedef enum {
  PREC_LOWEST,
  PREC_ASSIGN,
  PREC_COMMA,
  PREC_BINOP,
  PREC_UNARY_PREFIX,
  PREC_UNARY_POSTFIX,
  PREC_MEMBER,
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
static const parselet_t parsetab[TOK_COUNT];

// keyword table
static const struct { const char* s; tok_t t; } keywordtab[] = {
  #define _(NAME, ...)
  #define KEYWORD(str, NAME) {str, NAME},
  #include "tokens.h"
  #undef _
  #undef KEYWORD
};

// last_resort_node is returned by mknode when memory allocation fails
static node_t last_resort_node = { .kind = NBAD };

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
  const u8* inp = p->scanner.inp; // save state to guarantee progress
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


static bool node_has_strval(const node_t* n) {
  return n->kind == NID || n->kind == NCOMMENT;
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


// static const char* node_name(const node_t* n) {
//   static const char* names[NODEKIND_COUNT] = {
//     #define _(NAME) #NAME,
//     FOREACH_NODE_KIND(_)
//     #undef _
//   };
//   return (n->kind < NODEKIND_COUNT) ? names[n->kind] : "?";
// }


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


static void unexpected(parser_t* p, const char* extramsg) {
  char descr[64];
  tok_t tok = currtok(p);
  tok_descrs(descr, sizeof(descr), tok, scanner_lit(&p->scanner));
  error(p, NULL, "unexpected %s%s", descr, extramsg);
}


static void expect_fail(parser_t* p, tok_t expecttok, const char* errmsg) {
  char want[64], got[64];
  tok_descrs(want, sizeof(want), expecttok, (slice_t){0});
  tok_descrs(got, sizeof(got), currtok(p), scanner_lit(&p->scanner));
  error(p, NULL, "expected %s %s, got %s", want, errmsg, got);
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


void node_free(memalloc_t ast_ma, node_t* n) {
  if (n == &last_resort_node)
    return;
  for (node_t* cn = n->children.head; cn;) {
    node_t* cn2 = cn->next;
    node_free(ast_ma, cn);
    cn = cn2;
  }
  if (node_has_strval(n)) {
    mem_t m = MEM(n->strval, strlen(n->strval));
    mem_free(ast_ma, &m);
  }
  mem_freet(ast_ma, n);
}


static char* mkcstr(parser_t* p, slice_t src) {
  char* s = mem_strdup(p->ast_ma, src);
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


static node_t* addchild(node_t* restrict parent, node_t* restrict child) {
  assert(parent != child);
  if (parent->children.tail) {
    parent->children.tail->next = child;
  } else {
    parent->children.head = child;
  }
  parent->children.tail = child;
  assert(child->next == NULL);
  return parent;
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


static node_t* parse(parser_t* p, precedence_t prec) {
  tok_t tok = currtok(p);
  const parselet_t* parselet = &parsetab[tok];

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
    parselet = &parsetab[tok];
    if (parselet->infix == NULL || parselet->prec < prec) {
      if (parselet->infix) {
        LOG_PRATT("INFIX %s skip; parsetab[%u].prec < caller_prec (%d < %d)",
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


static node_t* id(parser_t* p, const char* helpmsg) {
  node_t* n = mknode(p, NID);
  n->strval = mkcstr(p, scanner_lit(&p->scanner));
  expect(p, TID, helpmsg);
  return n;
}


// fundef = "fun" name "(" params? ")" result? "{" body "}"
// result = params
// body   = (stmt ";")*
static node_t* prefix_fun(parser_t* p, precedence_t prec) {
  node_t* n = mknode(p, NFUN);
  next(p);

  node_t* name = id(p, "after fun");

  //expect(p, TID, "after fun");

  dlog("TODO rest of fun");

  return n;
}


static const parselet_t parsetab[TOK_COUNT] = {
  [TFUN] = {prefix_fun, NULL, PREC_LOWEST},
};


node_t* parser_parse(parser_t* p, memalloc_t ast_ma, input_t* input) {
  p->ast_ma = ast_ma;
  scanner_set_input(&p->scanner, input);
  node_t* unit = mknode(p, NUNIT);
  next(p);

  while (currtok(p) != TEOF) {
    node_t* n = parse(p, PREC_LOWEST);
    addchild(unit, n);
    if (currtok(p) == TSEMI) {
      unexpected(p, " at top-level");
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
