// Parser
// SPDX-License-Identifier: Apache-2.0
#include "colib.h"
#include "compiler.h"
#include "abuf.h"

#include <stdlib.h>
#include <strings.h> // strncasecmp


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
  PREC_UNARY_PREFIX,  // ++  --  +  -  !  ~  *  &  ?
  PREC_UNARY_POSTFIX, // ++  --  ()  []
  PREC_MEMBER,        // .

  PREC_LOWEST = PREC_COMMA,
} prec_t;


#define trace(fmt, va...) \
  _trace(opt_trace_parse, 2, "P", "%*s" fmt, p->traceindent*2, "", ##va)


#ifdef DEBUG
  static void _traceindent_decr(parser_t** cp) { (*cp)->traceindent--; }
  #define TRACE_SCOPE() \
    p->traceindent++; \
    parser_t* __tracep __attribute__((__cleanup__(_traceindent_decr),__unused__)) = p

  #define LOG_PRATT(p, msg) \
    trace("%s (%u:%u) %s", tok_name(p->scanner.tok), \
      loc_line(p->scanner.loc), loc_col(p->scanner.loc), msg); \
    TRACE_SCOPE()

  static void log_pratt_infix(
    parser_t* p, const char* class,
    const void* nullable parselet_infix, prec_t parselet_prec,
    prec_t ctx_prec)
  {
    if (!opt_trace_parse)
      return;
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
    return LOG_PRATT(p, buf);
  }
#else
  #define TRACE_SCOPE()     ((void)0)
  #define log_pratt_infix(...) ((void)0)
  #define LOG_PRATT(...) ((void)0)
#endif


#define PARGS   p, pl

typedef struct parselet parselet_t;

typedef stmt_t*(*prefix_stmt_parselet_t)(parser_t* p);
typedef stmt_t*(*infix_stmt_parselet_t)(parser_t* p, prec_t prec, stmt_t* left);

typedef expr_t*(*prefix_parselet_t)(parser_t*, const parselet_t* pl, nodeflag_t);
typedef expr_t*(*infix_parselet_t)(parser_t*, const parselet_t* pl, expr_t*, nodeflag_t);

typedef type_t*(*prefix_type_parselet_t)(parser_t* p);
typedef type_t*(*infix_type_parselet_t)(parser_t* p, prec_t prec, type_t* left);

typedef struct {
  prefix_stmt_parselet_t nullable prefix;
  infix_stmt_parselet_t  nullable infix;
  prec_t                          prec;
} stmt_parselet_t;

typedef struct parselet {
  prefix_parselet_t nullable prefix;
  infix_parselet_t  nullable infix;
  prec_t                     prec;
  op_t                       op;
} parselet_t;

typedef struct {
  prefix_type_parselet_t nullable prefix;
  infix_type_parselet_t  nullable infix;
  prec_t                          prec;
} type_parselet_t;

// parselet table (defined towards end of file)
static const stmt_parselet_t stmt_parsetab[TOK_COUNT];
static const parselet_t expr_parsetab[TOK_COUNT];
static const type_parselet_t type_parsetab[TOK_COUNT];

// last_resort_node is returned by mknode when memory allocation fails
static struct { node_t; u8 opaque[64]; } _last_resort_node = { .kind=NODE_BAD };
node_t* last_resort_node = (node_t*)&_last_resort_node;


inline static scanstate_t save_scanstate(parser_t* p) {
  return *(scanstate_t*)&p->scanner;
}

inline static void restore_scanstate(parser_t* p, scanstate_t state) {
  *(scanstate_t*)&p->scanner = state;
}


inline static locmap_t* locmap(parser_t* p) {
  return &p->scanner.compiler->locmap;
}


inline static tok_t currtok(parser_t* p) {
  return p->scanner.tok;
}

inline static loc_t currloc(parser_t* p) {
  return p->scanner.loc;
}

inline static origin_t curr_origin(parser_t* p) {
  return origin_make(locmap(p), currloc(p));
}


static void next(parser_t* p) {
  scanner_next(&p->scanner);
}


static bool slice_eq_cstri(slice_t s, const char* cstr) {
  usize len = strlen(cstr);
  return s.len == len && strncasecmp(s.chars, cstr, len) == 0;
}


// static tok_t lookahead(parser_t* p, u32 distance) {
//   assert(distance > 0);
//   scanstate_t scanstate = save_scanstate(p);
//   while (distance--)
//     next(p);
//   tok_t tok = currtok(p);
//   restore_scanstate(p, scanstate);
//   return tok;
// }


static bool lookahead_issym(parser_t* p, sym_t sym) {
  scanstate_t scanstate = save_scanstate(p);
  next(p);
  bool ok = currtok(p) == TID && p->scanner.sym == sym;
  restore_scanstate(p, scanstate);
  return ok;
}


// fastforward advances the scanner until one of the tokens in stoplist is encountered.
// The stoplist token encountered is consumed.
// stoplist should be NULL-terminated.
static void fastforward(parser_t* p, const tok_t* stoplist) {
  while (currtok(p) != TEOF) {
    const tok_t* tp = stoplist;
    while (*tp) {
      if (*tp++ == currtok(p))
        return;
    }
    next(p);
  }
}

static void fastforward_semi(parser_t* p) {
  fastforward(p, (const tok_t[]){ TSEMI, 0 });
}


// const origin_t to_origin(parser_t* p, T origin)
// where T is one of: origin_t | loc_t | node_t* (default)
#define to_origin(p, origin) ({ \
  __typeof__(origin) __tmp1 = origin; \
  __typeof__(origin)* __tmp = &__tmp1; \
  const origin_t __origin = _Generic(__tmp, \
          origin_t*:  *(origin_t*)__tmp, \
    const origin_t*:  *(origin_t*)__tmp, \
          loc_t*:     origin_make(locmap(p), *(loc_t*)__tmp), \
    const loc_t*:     origin_make(locmap(p), *(loc_t*)__tmp), \
    default: (*__tmp ? node_origin(locmap(p), *(node_t**)__tmp) : curr_origin(p)) \
  ); \
  __origin; \
})


// void diag(parser_t* p, T origin, diagkind_t diagkind, const char* fmt, ...)
// where T is one of: origin_t | loc_t | node_t* | expr_t*
#define diag(p, origin, diagkind, fmt, args...) \
  report_diag((p)->scanner.compiler, to_origin((p), (origin)), (diagkind), (fmt), ##args)

#define error(p, origin, fmt, args...)    diag(p, origin, DIAG_ERR, (fmt), ##args)
#define warning(p, origin, fmt, args...)  diag(p, origin, DIAG_WARN, (fmt), ##args)
#define help(p, origin, fmt, args...)     diag(p, origin, DIAG_HELP, (fmt), ##args)


// ATTR_FORMAT(printf,3,4)
// static void error_loc(parser_t* p, loc_t loc, const char* fmt, ...) {
//   if (p->scanner.inp == p->scanner.inend && p->scanner.tok == TEOF)
//     return;
//   origin_t origin = origin_make(locmap(p), loc);
//   va_list ap;
//   va_start(ap, fmt);
//   report_diagv(p->scanner.compiler, origin, DIAG_ERR, fmt, ap);
//   va_end(ap);
// }


// ATTR_FORMAT(printf,3,4)
// static void error(parser_t* p, const void* nullable n, const char* fmt, ...) {
//   if (p->scanner.inp == p->scanner.inend && p->scanner.tok == TEOF)
//     return;
//   origin_t origin = n ? node_origin(locmap(p), n) : curr_origin(p);
//   va_list ap;
//   va_start(ap, fmt);
//   report_diagv(p->scanner.compiler, origin, DIAG_ERR, fmt, ap);
//   va_end(ap);
// }


// ATTR_FORMAT(printf,3,4)
// static void warning(parser_t* p, const void* nullable n, const char* fmt, ...) {
//   origin_t origin = n ? node_origin(locmap(p), n) : curr_origin(p);
//   va_list ap;
//   va_start(ap, fmt);
//   report_diagv(p->scanner.compiler, origin, DIAG_WARN, fmt, ap);
//   va_end(ap);
// }


static void stop_parsing(parser_t* p) {
  stop_scanning(&p->scanner);
}


static void out_of_mem(parser_t* p) {
  error(p, NULL, "out of memory");
  stop_parsing(p); // end scanner, making sure we don't keep going
}


static const char* fmttok(parser_t* p, u32 bufindex, tok_t tok, slice_t lit) {
  buf_t* buf = tmpbuf(bufindex);
  buf_reserve(buf, 64);
  tok_descr(buf->p, buf->cap, tok, lit);
  return buf->chars;
}


static const char* fmtnode(parser_t* p, u32 bufindex, const void* n) {
  buf_t* buf = tmpbuf(bufindex);
  err_t err = node_fmt(buf, n, /*depth*/0);
  if (!err)
    return buf->chars;
  dlog("node_fmt: %s", err_str(err));
  out_of_mem(p);
  return "?";
}


static void unexpected(parser_t* p, const char* errmsg) {
  if (currtok(p) == TEOF && p->scanner.compiler->errcount > 0)
    return;
  const char* tokstr = fmttok(p, 0, currtok(p), scanner_lit(&p->scanner));
  int msglen = (int)strlen(errmsg);
  if (msglen && *errmsg != ',' && *errmsg != ';')
    msglen++;
  error(p, NULL, "unexpected %s%*s", tokstr, msglen, errmsg);
}


static void expect_fail(parser_t* p, tok_t expecttok, const char* errmsg) {
  const char* want = fmttok(p, 0, expecttok, (slice_t){0});
  const char* got = fmttok(p, 1, currtok(p), scanner_lit(&p->scanner));
  int msglen = (int)strlen(errmsg);
  if (msglen && *errmsg != ',' && *errmsg != ';')
    msglen++;
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


static bool expect2(parser_t* p, tok_t tok, const char* errmsg) {
  if LIKELY(currtok(p) == tok) {
    next(p);
    return true;
  }
  unexpected(p, errmsg);
  fastforward(p, (const tok_t[]){ tok, TSEMI, 0 });
  if (currtok(p) == tok)
    next(p);
  return false;
}


#define mknode(p, TYPE, kind)      ( (TYPE*)_mknode((p), sizeof(TYPE), (kind)) )
#define mkexpr(p, TYPE, kind, fl)  ( (TYPE*)_mkexpr((p), sizeof(TYPE), (kind), (fl)) )


node_t* _mknode(parser_t* p, usize size, nodekind_t kind) {
  mem_t m = mem_alloc_zeroed(p->ast_ma, size);
  if UNLIKELY(m.p == NULL)
    return out_of_mem(p), last_resort_node;
  node_t* n = m.p;
  n->kind = kind;
  n->loc = currloc(p);
  return n;
}


static expr_t* _mkexpr(parser_t* p, usize size, nodekind_t kind, nodeflag_t fl) {
  assertf(nodekind_isexpr(kind), "%s", nodekind_name(kind));
  expr_t* n = (expr_t*)_mknode(p, size, kind);
  n->flags = fl;
  n->type = type_unknown;
  return n;
}


static void* mkbad(parser_t* p) {
  expr_t* n = (expr_t*)mknode(p, __typeof__(_last_resort_node), NODE_BAD);
  n->type = type_void;
  return n;
}


static reftype_t* mkreftype(parser_t* p, bool ismut) {
  reftype_t* t = mknode(p, reftype_t, ismut ? TYPE_MUTREF : TYPE_REF);
  t->size = p->scanner.compiler->ptrsize;
  t->align = t->size;
  return t;
}


static type_t* mkunresolvedtype(parser_t* p, sym_t name, loc_t loc) {
  unresolvedtype_t* t = mknode(p, unresolvedtype_t, TYPE_UNRESOLVED);
  t->flags |= NF_UNKNOWN;
  t->name = name;
  if (loc)
    t->loc = loc;
  return (type_t*)t;
}


node_t* clone_node(parser_t* p, const node_t* n) {
  switch (n->kind) {
  case EXPR_FIELD:
  case EXPR_PARAM:
  case EXPR_LET:
  case EXPR_VAR:
    return (node_t*)CLONE_NODE(p, (local_t*)n);
  default:
    panic("TODO %s %s", __FUNCTION__, nodekind_name(n->kind));
  }
}


// —————————————————————————————————————————————————————————————————————————————————————


static void enter_scope(parser_t* p) {
  if (!scope_push(&p->scope, p->ma))
    out_of_mem(p);
}


static void leave_scope(parser_t* p) {
  scope_pop(&p->scope);
}


static node_t* nullable lookup(parser_t* p, sym_t name) {
  node_t* n = scope_lookup(&p->scope, name, U32_MAX);
  if (!n) {
    // look in package scope and its parent universe scope
    void** vp = map_lookup_ptr(&p->pkgdefs, name);
    if (!vp)
      return NULL;
    n = *vp;
  }
  return n;
}


static void define_replace(parser_t* p, sym_t name, node_t* n) {
  trace("redefine %s %s", name, nodekind_name(n->kind));
  assert(n->kind != EXPR_ID);
  assert(name != sym__);
  if UNLIKELY(!scope_define(&p->scope, p->ma, name, n))
    out_of_mem(p);
  if (scope_istoplevel(&p->scope)) {
    void** vp = map_assign_ptr(&p->pkgdefs, p->ma, name);
    if UNLIKELY(!vp)
      return out_of_mem(p);
    *vp = n;
  }
}


static void define(parser_t* p, sym_t name, node_t* n) {
  if (name == sym__)
    return;

  trace("define %s %s", name, nodekind_name(n->kind));

  node_t* existing = scope_lookup(&p->scope, name, 0);
  if (existing)
    goto err_duplicate;

  if (!scope_define(&p->scope, p->ma, name, n))
    out_of_mem(p);

  // top-level definitions also goes into package scope
  if (scope_istoplevel(&p->scope)) {
    // trace("define in pkg %s %s", name, nodekind_name(n->kind));
    void** vp = map_assign_ptr(&p->pkgdefs, p->ma, name);
    if (!vp)
      return out_of_mem(p);
    if (*vp) {
      existing = *vp;
      goto err_duplicate;
    }
    *vp = n;
  }
  return;
err_duplicate:
  error(p, n, "redefinition of \"%s\"", name);
  if (loc_line(existing->loc))
    help(p, existing, "\"%s\" previously defined here", name);
}


// —————————————————————————————————————————————————————————————————————————————————————


static void push_child(parser_t* p, ptrarray_t* children, void* child) {
  if UNLIKELY(!ptrarray_push(children, p->ast_ma, child))
    out_of_mem(p);
}


static void dotctx_push(parser_t* p, expr_t* nullable n) {
  if UNLIKELY(!ptrarray_push(&p->dotctxstack, p->ma, p->dotctx))
    out_of_mem(p);
  p->dotctx = n;
}

static void dotctx_pop(parser_t* p) {
  assert(p->dotctxstack.len > 0);
  p->dotctx = ptrarray_pop(&p->dotctxstack);
}


static stmt_t* stmt(parser_t* p) {
  tok_t tok = currtok(p);
  const stmt_parselet_t* parselet = &stmt_parsetab[tok];
  LOG_PRATT(p, "prefix stmt");
  if UNLIKELY(!parselet->prefix) {
    unexpected(p, "where a statement is expected");
    stmt_t* n = mkbad(p);
    stop_parsing(p);
    return n;
  }
  stmt_t* n = parselet->prefix(p);
  for (;;) {
    tok = currtok(p);
    parselet = &stmt_parsetab[tok];
    if (parselet->infix == NULL || parselet->prec < PREC_LOWEST)
      return n;
    log_pratt_infix(p, "stmt", parselet->infix, parselet->prec, PREC_LOWEST);
    n = parselet->infix(p, PREC_LOWEST, n);
  }
}


static expr_t* expr_infix(parser_t* p, prec_t prec, expr_t* n, nodeflag_t fl) {
  for (;;) {
    const parselet_t*parselet = &expr_parsetab[currtok(p)];
    if (parselet->infix == NULL || parselet->prec < prec)
      return n;
    log_pratt_infix(p, "expr", parselet->infix, parselet->prec, prec);
    n = parselet->infix(p, parselet, n, fl);
  }
}


static expr_t* expr(parser_t* p, prec_t prec, nodeflag_t fl) {
  const parselet_t* parselet = &expr_parsetab[currtok(p)];
  LOG_PRATT(p, "prefix expr");
  if UNLIKELY(!parselet->prefix) {
    unexpected(p, "where an expression is expected");
    fastforward_semi(p);
    return mkbad(p);
  }
  expr_t* n = parselet->prefix(p, parselet, fl);
  return expr_infix(p, prec, n, fl);
  // for (;;) {
  //   parselet = &expr_parsetab[currtok(p)];
  //   if (parselet->infix == NULL || parselet->prec < prec)
  //     return n;
  //   log_pratt_infix(p, "expr", parselet->infix, parselet->prec, prec);
  //   n = parselet->infix(p, parselet, n, fl);
  // }
}


static type_t* type(parser_t* p, prec_t prec) {
  tok_t tok = currtok(p);
  const type_parselet_t* parselet = &type_parsetab[tok];
  LOG_PRATT(p, "prefix type");
  if UNLIKELY(!parselet->prefix) {
    unexpected(p, "where type is expected");
    fastforward_semi(p);
    return type_void;
  }
  type_t* t = parselet->prefix(p);
  for (;;) {
    tok = currtok(p);
    parselet = &type_parsetab[tok];
    if (parselet->infix == NULL || parselet->prec < prec)
      return t;
    log_pratt_infix(p, "type", parselet->infix, parselet->prec, prec);
    t = parselet->infix(p, prec, t);
  }
}


static type_t* named_type(parser_t* p, sym_t name, loc_t loc) {
  const node_t* t = lookup(p, name);
  if UNLIKELY(!t || !node_istype(t))
    return mkunresolvedtype(p, name, loc);
  return (type_t*)t;
}


static type_t* type_id(parser_t* p) {
  type_t* t = named_type(p, p->scanner.sym, 0);
  next(p);
  return t;
}


// array_type = "[" type constexpr? "]"
static type_t* type_array(parser_t* p) {
  arraytype_t* at = mknode(p, arraytype_t, TYPE_ARRAY);
  next(p);
  at->elem = type(p, PREC_COMMA);
  if (currtok(p) != TRBRACK && currtok(p) != TSEMI)
    at->lenexpr = expr(p, PREC_COMMA, NF_RVALUE);
  at->endloc = currloc(p);
  expect(p, TRBRACK, "to match [");
  return (type_t*)at;
}


fun_t* nullable lookup_typefun(parser_t* p, type_t* recv, sym_t name) {
  // find function map for recv
  void** mmp = map_lookup_ptr(&p->recvtmap, recv);
  if (!mmp)
    return NULL; // no functions on recv
  map_t* mm = assertnotnull(*mmp);

  // find function by name
  void** mp = map_lookup_ptr(mm, name);
  return mp ? assertnotnull(*mp) : NULL;
}


local_t* nullable lookup_struct_field(structtype_t* st, sym_t name) {
  ptrarray_t fields = st->fields;
  for (u32 i = 0; i < fields.len; i++) {
    local_t* f = (local_t*)fields.v[i];
    if (f->name == name)
      return f;
  }
  return NULL;
}


// field = id ("," id)* type ("=" expr ("," expr))
static bool struct_fieldset(parser_t* p, structtype_t* st) {
  u32 fields_start = st->fields.len;
  for (;;) {
    local_t* f = mknode(p, local_t, EXPR_FIELD);
    f->name = p->scanner.sym;

    if UNLIKELY(!expect(p, TID, "")) {
      fastforward_semi(p);
      return false;
    }

    node_t* existing = (node_t*)lookup_struct_field(st, f->name);
    if LIKELY(!existing)
      existing = (node_t*)lookup_typefun(p, (type_t*)st, f->name);
    if UNLIKELY(existing) {
      const char* s = fmtnode(p, 0, st);
      error(p, f, "duplicate %s \"%s\" for type %s",
        (existing->kind == EXPR_FIELD) ? "field" : "member", f->name, s);
      if (loc_line(existing->loc))
        warning(p, (node_t*)existing, "previously defined here");
    }

    push_child(p, &st->fields, f);
    if (currtok(p) != TCOMMA)
      break;
    next(p);
  }

  type_t* t = type(p, PREC_MEMBER);
  for (u32 i = fields_start; i < st->fields.len; i++) {
    local_t* f = st->fields.v[i];
    f->type = t;
    bubble_flags(f, t);
    bubble_flags(st, f);
  }

  if (currtok(p) != TASSIGN)
    return false;

  next(p);
  u32 i = fields_start;
  for (;;) {
    if (i == st->fields.len) {
      error(p, NULL, "excess field initializer");
      expr(p, PREC_COMMA, NF_RVALUE);
      break;
    }
    local_t* f = st->fields.v[i++];
    f->init = expr(p, PREC_COMMA, NF_RVALUE);
    bubble_flags(f, f->init);
    bubble_flags(st, f->init);
    if (currtok(p) != TCOMMA)
      break;
    next(p);
  }
  if (i < st->fields.len)
    error(p, NULL, "missing field initializer");
  return true;
}


static fun_t* fun(parser_t*, nodeflag_t, type_t* nullable recvt, bool requirename);

// struct parser that allows inline "fun" definitions in struct
// static type_t* type_struct1(parser_t* p, structtype_t* st) {
//   while (currtok(p) != TRBRACE) {
//     if (currtok(p) == TFUN)
//       goto funs;
//     st->hasinit |= struct_fieldset(p, st);
//     if (currtok(p) != TSEMI)
//       break;
//     next(p);
//   }
//   goto end_fields_and_funs;
// funs:
//   assert(currtok(p) == TFUN);
//   u32 first_fn_index = p->unit->children.len;
//   fun_t* fn;
//   for (;;) {
//     fn = fun(p, 0, /*recvt*/(type_t*)st, /*requirename*/true);
//     // Append the function to the unit, not to the structtype.
//     // This simplifies both the general implementation and code generation.
//     push_child(p, &p->unit->children, fn);
//     bubble_flags(p->unit, fn);
//     if (currtok(p) != TSEMI)
//       break;
//     next(p);
//     if UNLIKELY(currtok(p) != TFUN) {
//       if (currtok(p) != TID)
//         break;
//       fun_t* fn1 = p->unit->children.v[first_fn_index];
//       error(p, NULL, "fields cannot be defined after type functions");
//       if (loc_line(fn1->loc)) {
//         help(p, fn1->loc, "define the field \"%s\" before this function",
//           p->scanner.sym);
//       } else if (loc_line(fn->loc)) {
//         help(p, fn->loc, "a function was defined here");
//       }
//       fastforward(p, (const tok_t[]){ TRBRACE, 0 });
//       break;
//     }
//   }
// end_fields_and_funs:
//   expect(p, TRBRACE, "to end struct");
//   for (u32 i = 0; i < st->fields.len; i++) {
//     local_t* f = st->fields.v[i];
//     type_t* ft = assertnotnull(f->type);
//     st->align = MAX(st->align, ft->align);
//     st->size += ft->size;
//   }
//   st->size = ALIGN2(st->size, st->align);
//   return (type_t*)st;
// }


// struct parser that prohibits inline "fun" definitions in struct
static type_t* type_struct1(parser_t* p, structtype_t* st) {
  bool reported_fun = false;
  while (currtok(p) != TRBRACE) {
    if UNLIKELY(currtok(p) == TFUN) {
      if (!reported_fun) {
        reported_fun = true;
        error(p, NULL, "functions are not allowed in struct definitions");
      }
      fun(p, 0, /*recvt*/(type_t*)st, /*requirename*/false);
      if (!expect(p, TSEMI, ""))
        break;
      continue;
    }
    st->hasinit |= struct_fieldset(p, st);
    if (currtok(p) != TSEMI)
      break;
    next(p);
  }
  expect(p, TRBRACE, "to match {");
  return (type_t*)st;
}


static type_t* type_struct(parser_t* p) {
  structtype_t* st = mknode(p, structtype_t, TYPE_STRUCT);
  next(p);
  return type_struct1(p, st);
}


// ptr_type = "*" type
static type_t* type_ptr(parser_t* p) {
  ptrtype_t* t = mknode(p, ptrtype_t, TYPE_PTR);
  next(p);
  t->size = p->scanner.compiler->ptrsize;
  t->align = t->size;
  t->elem = type(p, PREC_UNARY_PREFIX);
  bubble_flags(t, t->elem);
  return (type_t*)t;
}


// ref_type   = slice_type | ref_type1
// slice_type = "mut"? "&" "[" type "]"
// ref_type1  = "mut"? "&" type
static type_t* type_ref1(parser_t* p, bool ismut) {
  reftype_t* t = mkreftype(p, ismut);
  next(p);
  t->elem = type(p, PREC_UNARY_PREFIX);
  if (t->elem->kind == TYPE_ARRAY && ((arraytype_t*)t->elem)->lenexpr == NULL) {
    // "&[T]" is a slice
    assert(((arraytype_t*)t->elem)->len == 0);
    static_assert(sizeof(arraytype_t) >= sizeof(slicetype_t), "convertible");

    // convert array type to slice type
    arraytype_t* at = (arraytype_t*)t->elem;
    loc_t endloc = at->endloc;
    type_t* elem = at->elem;

    slicetype_t* st = (slicetype_t*)at;
    st->kind = ismut ? TYPE_MUTSLICE : TYPE_SLICE;
    st->endloc = endloc;
    st->elem = elem;

    return (type_t*)st;
  }
  bubble_flags(t, t->elem);
  return (type_t*)t;
}

// "&" type
static type_t* type_ref(parser_t* p) {
  return type_ref1(p, /*ismut*/false);
}

// "mut" "&" type
static type_t* type_mut(parser_t* p) {
  next(p);
  if UNLIKELY(currtok(p) != TAND) {
    unexpected(p, "expecting '&'");
    return mkbad(p);
  }
  return type_ref1(p, /*ismut*/true);
}


// optional_type = "?" type
static type_t* type_optional(parser_t* p) {
  opttype_t* t = mknode(p, opttype_t, TYPE_OPTIONAL);
  next(p);
  t->elem = type(p, PREC_UNARY_PREFIX);
  bubble_flags(t, t->elem);
  return (type_t*)t;
}


// typedef = "type" id type
static stmt_t* stmt_typedef(parser_t* p) {
  typedef_t* n = mknode(p, typedef_t, STMT_TYPEDEF);
  next(p);

  n->type.kind = TYPE_ALIAS;
  n->type.loc = currloc(p);

  sym_t name = p->scanner.sym;
  if (expect(p, TID, ""))
    define(p, name, (node_t*)&n->type);

  // special path for struct to avoid (typedef (alias x (struct x))),
  // instead we simply get (typedef (struct x))
  if (currtok(p) == TLBRACE) {
    n->type.kind = TYPE_STRUCT;
    n->type.loc = currloc(p);
    next(p);
    n->structtype.name = name;
    type_struct1(p, &n->structtype);
    bubble_flags(n, &n->structtype);
  } else {
    n->aliastype.name = name;
    n->aliastype.elem = type(p, PREC_COMMA);
    bubble_flags(n, n->aliastype.elem);
    if UNLIKELY(type_isopt(n->aliastype.elem)) {
      error(p, n->aliastype.elem,
        "cannot define optional aliased type;"
        " instead, mark as optional at use sites with ?%s", name);
    }
    if (n->aliastype.elem->kind == TYPE_STRUCT)
      ((structtype_t*)n->aliastype.elem)->name = n->aliastype.name;
  }

  return (stmt_t*)n;
}


static bool resolve_id(parser_t* p, idexpr_t* n) {
  n->ref = lookup(p, n->name);
  if (!n->ref) {
    trace("identifier \"%s\" not yet known", n->name);
    n->type = type_unknown;
  } else if (node_isexpr(n->ref)) {
    n->type = ((expr_t*)n->ref)->type;
  } else if (nodekind_istype(n->ref->kind)) {
    n->type = (type_t*)n->ref;
  } else {
    error(p, n, "cannot use %s \"%s\" as an expression",
      nodekind_fmt(n->ref->kind), n->name);
    return false;
  }
  bubble_flags(n, n->type);
  return true;
}


static expr_t* expr_id(parser_t* p, const parselet_t* pl, nodeflag_t fl) {
  idexpr_t* n = mkexpr(p, idexpr_t, EXPR_ID, fl);
  n->name = p->scanner.sym;
  next(p);
  resolve_id(p, n);
  return (expr_t*)n;
}


static expr_t* expr_var(parser_t* p, const parselet_t* pl, nodeflag_t fl) {
  local_t* n = mkexpr(p, local_t, currtok(p) == TLET ? EXPR_LET : EXPR_VAR, fl);
  next(p);
  if (currtok(p) != TID) {
    unexpected(p, "expecting identifier");
    return mkbad(p);
  } else {
    n->name = p->scanner.sym;
    n->nameloc = currloc(p);
    next(p);
  }
  bool ok = true;
  if (currtok(p) == TASSIGN) {
    next(p);
    n->init = expr(p, PREC_ASSIGN, fl | NF_RVALUE);
    n->type = n->init->type;
    bubble_flags(n, n->init);
  } else {
    n->type = type(p, PREC_LOWEST);
    bubble_flags(n, n->type);
    if (currtok(p) == TASSIGN) {
      next(p);
      n->init = expr(p, PREC_ASSIGN, fl | NF_RVALUE);
      bubble_flags(n, n->init);
    }
  }

  define(p, n->name, (node_t*)n);

  // check for required initializer expression
  if (!n->init && ok) {
    if UNLIKELY(n->kind == EXPR_LET) {
      error(p, NULL, "missing value for let binding, expecting '='");
      ok = false;
    } else if UNLIKELY(n->type->kind == TYPE_REF) {
      error(p, NULL, "missing initial value for reference variable, expecting '='");
      ok = false;
    }
  }

  return (expr_t*)n;
}


static block_t* block(parser_t* p, nodeflag_t fl) {
  block_t* n = mkexpr(p, block_t, EXPR_BLOCK, fl);
  next(p);

  if ((fl & NF_RVALUE) == 0)
    n->type = type_void;

  fl &= ~NF_RVALUE;
  bool reported_unreachable = false;
  bool exited = false;

  if (currtok(p) != TRBRACE && currtok(p) != TEOF) {
    for (;;) {
      expr_t* cn = expr(p, PREC_LOWEST, fl);
      if UNLIKELY(!ptrarray_push(&n->children, p->ast_ma, cn)) {
        out_of_mem(p);
        break;
      }
      bubble_flags(n, cn);

      if (exited) {
        if (!reported_unreachable) {
          reported_unreachable = true;
          warning(p, (node_t*)cn, "unreachable code");
        }
      } else if (cn->kind == EXPR_RETURN) {
        exited = true;
      }

      if (currtok(p) != TSEMI)
        break;
      next(p); // consume ";"

      if (currtok(p) == TRBRACE || currtok(p) == TEOF)
        break;
    }
  }

  n->endloc = currloc(p);
  expect2(p, TRBRACE, ", expected '}' or ';'");

  return n;
}


static block_t* any_as_block(parser_t* p, nodeflag_t fl) {
  if (currtok(p) == TLBRACE)
    return block(p, fl);
  block_t* n = mkexpr(p, block_t, EXPR_BLOCK, fl);
  expr_t* cn = expr(p, PREC_COMMA, fl);
  if UNLIKELY(!ptrarray_push(&n->children, p->ast_ma, cn))
    out_of_mem(p);
  bubble_flags(n, cn);
  return n;
}


static expr_t* expr_block(parser_t* p, const parselet_t* pl, nodeflag_t fl) {
  enter_scope(p);
  block_t* n = block(p, fl);
  leave_scope(p);
  return (expr_t*)n;
}


static void narrow_optional_check(parser_t* p, expr_t* cond) {
  assert(type_isopt(cond->type));

  // apply negation, e.g. "if (!x)"
  bool neg = false;
  while (cond->kind == EXPR_PREFIXOP) {
    unaryop_t* op = (unaryop_t*)cond;
    if UNLIKELY(op->op != OP_NOT) {
      error(p, cond, "invalid operation %s on optional type", fmtnode(p, 0, cond));
      return;
    }
    neg = !neg;
    cond = assertnotnull(op->expr);
  }

  // effective_type is either T or void, depending on "!" prefix ops.
  // e.g. "var x ?T ... ; if (x) { ... /* x is definitely T here */ }"
  // e.g. "var x ?T ... ; if (!x) { ... /* x is definitely void here */ }"
  type_t* effective_type = neg ? type_void : ((opttype_t*)cond->type)->elem;

  // redefine with effective_type
  switch (cond->kind) {
    case EXPR_ID: {
      // e.g. "if x { ... }"
      idexpr_t* id = (idexpr_t*)cond;
      if (!node_isexpr(id->ref))
        return error(p, cond, "conditional is not an expression");

      idexpr_t* id2 = CLONE_NODE(p, id);
      id2->type = effective_type;

      expr_t* ref2 = (expr_t*)clone_node(p, id->ref);
      ref2->type = effective_type;
      define_replace(p, id->name, (node_t*)ref2);

      break;
    }
    case EXPR_LET:
    case EXPR_VAR: {
      // e.g. "if let x = expr { ... }"
      ((local_t*)cond)->type = effective_type;
      cond->flags |= NF_OPTIONAL;
      break;
    }
  }
}


static expr_t* expr_if(parser_t* p, const parselet_t* pl, nodeflag_t fl) {
  ifexpr_t* n = mkexpr(p, ifexpr_t, EXPR_IF, fl);
  next(p);

  // enter "cond" scope
  enter_scope(p);

  // condition
  n->cond = expr(p, PREC_COMMA, fl | NF_RVALUE);
  bubble_flags(n, n->cond);
  if (n->cond->type->kind == TYPE_OPTIONAL)
    narrow_optional_check(p, n->cond);

  // "then"
  enter_scope(p);
  n->thenb = any_as_block(p, fl);
  bubble_flags(n, n->thenb);
  leave_scope(p);

  // "else"
  if (currtok(p) == TELSE) {
    next(p);
    enter_scope(p);
    n->elseb = any_as_block(p, fl);
    bubble_flags(n, n->elseb);
    leave_scope(p);
  }

  // leave "cond" scope
  leave_scope(p);

  return (expr_t*)n;
}


// for       = "for" ( for_head | for_phead ) expr
// for_head  = ( expr | expr? ";" expr ";" expr? )
// for_phead = "(" for_head ")"
static expr_t* expr_for(parser_t* p, const parselet_t* pl, nodeflag_t fl) {
  forexpr_t* n = mkexpr(p, forexpr_t, EXPR_FOR, fl);
  next(p);

  bool paren = currtok(p) == TLPAREN;
  if (paren)
    next(p);

  if (currtok(p) == TSEMI) {
    // "for ; i < 4; i++"
    next(p);
    n->cond = expr(p, PREC_COMMA, fl);
    expect(p, TSEMI, "");
    next(p);
    n->end = expr(p, PREC_COMMA, fl);
  } else {
    // "for i < 4"
    n->cond = expr(p, PREC_COMMA, fl);
    if (currtok(p) == TSEMI) {
      // "for i = 0; i < 4; i++"
      next(p);
      n->start = n->cond;
      n->cond = expr(p, PREC_COMMA, fl);
      expect(p, TSEMI, "");
      n->end = expr(p, PREC_COMMA, fl);
      bubble_flags(n, n->start);
      bubble_flags(n, n->end);
    }
  }
  if (paren)
    expect(p, TRPAREN, "");
  n->body = expr(p, PREC_COMMA, fl);
  bubble_flags(n, n->cond);
  bubble_flags(n, n->body);
  return (expr_t*)n;
}


// return = "return" (expr ("," expr)*)?
static expr_t* expr_return(parser_t* p, const parselet_t* pl, nodeflag_t fl) {
  retexpr_t* n = mkexpr(p, retexpr_t, EXPR_RETURN, fl);
  next(p);
  if (currtok(p) == TSEMI)
    return (expr_t*)n;
  n->value = expr(p, PREC_COMMA, fl | NF_RVALUE);
  n->type = n->value->type;
  bubble_flags(n, n->value);
  return (expr_t*)n;
}


static expr_t* intlit(parser_t* p, nodeflag_t fl, bool isneg) {
  intlit_t* n = mkexpr(p, intlit_t, EXPR_INTLIT, fl);
  n->intval = p->scanner.litint;
  loc_set_width(&n->loc, scanner_lit(&p->scanner).len);
  next(p);
  return (expr_t*)n;
}


static expr_t* floatlit(parser_t* p, nodeflag_t fl, bool isneg) {
  floatlit_t* n = mkexpr(p, floatlit_t, EXPR_FLOATLIT, fl);
  char* endptr = NULL;

  // note: scanner always starts float litbuf with '+'
  if (isneg)
    p->scanner.litbuf.chars[0] = '-';

  n->f64val = strtod(p->scanner.litbuf.chars, &endptr);
  if UNLIKELY(endptr != p->scanner.litbuf.chars + p->scanner.litbuf.len) {
    error(p, n, "invalid floating-point constant");
  }
  // note: typecheck checks for overflow (HUGE_VAL)

  next(p);
  return (expr_t*)n;
}


static expr_t* expr_intlit(parser_t* p, const parselet_t* pl, nodeflag_t fl) {
  return intlit(p, fl, /*isneg*/false);
}


static expr_t* expr_floatlit(parser_t* p, const parselet_t* pl, nodeflag_t fl) {
  return floatlit(p, fl, /*isneg*/false);
}


static expr_t* expr_strlit(parser_t* p, const parselet_t* pl, nodeflag_t fl) {
  strlit_t* n = mkexpr(p, strlit_t, EXPR_STRLIT, fl);

  slice_t str = scanner_strval(&p->scanner);
  n->bytes = (u8*)mem_strdup(p->ast_ma, str, 0);
  n->len = str.len;

  if UNLIKELY(!n->bytes) {
    out_of_mem(p);
    n->len = 0;
  }

  next(p);

  return (expr_t*)n;
}


static expr_t* expr_boollit(parser_t* p, const parselet_t* pl, nodeflag_t fl) {
  intlit_t* n = mkexpr(p, intlit_t, EXPR_BOOLLIT, fl | NF_CHECKED);
  n->intval = currtok(p) == TTRUE;
  n->type = type_bool;
  next(p);
  return (expr_t*)n;
}


// arraylit = "[" (expr ("," | ";"))* "]"
static expr_t* expr_arraylit(parser_t* p, const parselet_t* pl, nodeflag_t fl) {
  arraylit_t* n = mkexpr(p, arraylit_t, EXPR_ARRAYLIT, fl);
  next(p); // consume '['

  // parse values
  if (currtok(p) != TRBRACK) {
    fl |= NF_RVALUE;
    for (;;) {
      expr_t* val = expr(p, PREC_COMMA, fl);
      push_child(p, &n->values, val);
      bubble_flags(n, val);
      if (currtok(p) != TSEMI && currtok(p) != TCOMMA)
        break;
      next(p);
    }
  }

  // ']'
  n->endloc = currloc(p);
  expect(p, TRBRACK, "to end array literal");
  return (expr_t*)n;
}


static expr_t* expr_prefix_op(parser_t* p, const parselet_t* pl, nodeflag_t fl) {
  unaryop_t* n = mkexpr(p, unaryop_t, EXPR_PREFIXOP, fl);
  n->op = pl->op;
  next(p);
  fl |= NF_RVALUE;
  switch (currtok(p)) {
    // special case for negative number constants
    case TINTLIT:   n->expr = intlit(p, /*isneg*/n->op == OP_SUB, fl); break;
    case TFLOATLIT: n->expr = floatlit(p, /*isneg*/n->op == OP_SUB, fl); break;
    default:        n->expr = expr(p, PREC_UNARY_PREFIX, fl);
  }
  n->type = n->expr->type;
  bubble_flags(n, n->expr);
  return (expr_t*)n;
}


// expr_postfix_op = expr ("++" | "--")
static expr_t* expr_postfix_op(
  parser_t* p, const parselet_t* pl, expr_t* left, nodeflag_t fl)
{
  unaryop_t* n = mkexpr(p, unaryop_t, EXPR_POSTFIXOP, fl);
  n->op = pl->op;
  next(p);
  n->expr = left;
  n->type = left->type;
  bubble_flags(n, n->expr);
  return (expr_t*)n;
}


static expr_t* expr_infix_op(
  parser_t* p, const parselet_t* pl, expr_t* left, nodeflag_t fl)
{
  binop_t* n = mkexpr(p, binop_t, EXPR_BINOP, fl);
  n->op = pl->op;
  next(p);

  left->flags |= NF_RVALUE;
  n->left = left;

  n->right = expr(p, pl->prec, fl | NF_RVALUE);

  n->type = left->type;
  bubble_flags(n, n->left);
  bubble_flags(n, n->right);
  return (expr_t*)n;
}


static expr_t* expr_assign_op(
  parser_t* p, const parselet_t* pl, expr_t* left, nodeflag_t fl)
{
  expr_t* n = expr_infix_op(p, pl, left, fl);
  left->flags &= ~NF_RVALUE;
  n->kind = EXPR_ASSIGN;
  return n;
}


static expr_t* expr_cmp_op(
  parser_t* p, const parselet_t* pl, expr_t* left, nodeflag_t fl)
{
  expr_t* n = expr_infix_op(p, pl, left, fl);
  n->type = type_bool;
  return n;
}


static bool expr_isstorage(const expr_t* n) {
  switch (n->kind) {
  case EXPR_ID: {
    const idexpr_t* id = (const idexpr_t*)n;
    return id->ref && nodekind_isexpr(id->ref->kind) && expr_isstorage((expr_t*)id->ref);
  }
  case EXPR_MEMBER:
  case EXPR_PARAM:
  case EXPR_LET:
  case EXPR_VAR:
  case EXPR_FUN:
  case EXPR_DEREF:
    return true;
  case EXPR_PREFIXOP:
    return ((unaryop_t*)n)->op == OP_DEREF;
  default:
    return false;
  }
}


// expr_ismut returns true if n is something that can be mutated
static bool expr_ismut(const expr_t* n) {
  assert(expr_isstorage(n));
  switch (n->kind) {
  case EXPR_ID: {
    const idexpr_t* id = (const idexpr_t*)n;
    return id->ref && nodekind_isexpr(id->ref->kind) && expr_ismut((expr_t*)id->ref);
  }
  case EXPR_MEMBER: {
    const member_t* m = (const member_t*)n;
    return expr_ismut(m->target) && expr_ismut(m->recv);
  }
  case EXPR_PARAM:
  case EXPR_VAR:
    return true;
  default:
    return false;
  }
  return true;
}


// deref_expr = "*" expr
static expr_t* expr_deref(parser_t* p, const parselet_t* pl, nodeflag_t fl) {
  unaryop_t* n = mkexpr(p, unaryop_t, EXPR_DEREF, fl);
  n->op = pl->op;
  next(p);
  n->expr = expr(p, PREC_UNARY_PREFIX, fl);
  bubble_flags(n, n->expr);

  if (type_isptrlike(n->expr->type))
    n->type = ((ptrtype_t*)n->expr->type)->elem;
  // else: let typecheck do its thing

  return (expr_t*)n;
}


// mut_expr|ref_expr = "mut"? "&" expr
static expr_t* expr_ref1(parser_t* p, nodeflag_t fl, bool ismut) {
  unaryop_t* n = mkexpr(p, unaryop_t, EXPR_PREFIXOP, fl);
  n->op = OP_AND;
  next(p);
  n->expr = expr(p, PREC_UNARY_PREFIX, fl | NF_RVALUE);
  bubble_flags(n, n->expr);

  if UNLIKELY(n->expr->type->kind == TYPE_REF) {
    const char* ts = fmtnode(p, 0, n->expr->type);
    error(p, n, "referencing reference type %s", ts);
  } else if UNLIKELY(!expr_isstorage(n->expr)) {
    const char* ts = fmtnode(p, 0, n->expr->type);
    error(p, n, "referencing ephemeral value of type %s", ts);
  } else if UNLIKELY(ismut && !expr_ismut(n->expr)) {
    const char* s = fmtnode(p, 0, n->expr);
    nodekind_t k = n->expr->kind;
    if (k == EXPR_ID)
      k = ((idexpr_t*)n->expr)->ref->kind;
    error(p, n, "mutable reference to immutable %s %s", nodekind_fmt(k), s);
  }

  reftype_t* t = mkreftype(p, ismut);
  t->elem = n->expr->type;
  bubble_flags(t, t->elem);

  n->type = (type_t*)t;
  return (expr_t*)n;
}

// ref_expr = "&" expr
static expr_t* expr_ref(parser_t* p, const parselet_t* pl, nodeflag_t fl) {
  return expr_ref1(p, fl, /*ismut*/false);
}

// mut_expr = "mut" "&" expr
static expr_t* expr_mut(parser_t* p, const parselet_t* pl, nodeflag_t fl) {
  next(p);
  if UNLIKELY(currtok(p) != TAND) {
    unexpected(p, "expecting '&'");
    return mkbad(p);
  }
  return expr_ref1(p, fl, /*ismut*/true);
}


// group = "(" expr ")"
static expr_t* expr_group(parser_t* p, const parselet_t* pl, nodeflag_t fl) {
  next(p);
  expr_t* n = expr(p, PREC_COMMA, fl);
  if (currtok(p) == TCOMMA)
    dlog("TODO tuple");
  expect(p, TRPAREN, "");
  return n;
}


// named_param_or_id = id ":" expr | id
static expr_t* named_param_or_id(parser_t* p, nodeflag_t fl) {
  assert(currtok(p) == TID);

  usize nodesize = MAX(sizeof(idexpr_t), sizeof(local_t));
  idexpr_t* n = (idexpr_t*)_mkexpr(p, nodesize, EXPR_ID, fl);
  n->name = p->scanner.sym;
  next(p);

  if (currtok(p) != TCOLON) {
    resolve_id(p, n);
    return expr_infix(p, PREC_COMMA, (expr_t*)n, fl);
  }

  next(p);
  sym_t name = n->name;
  local_t* local = (local_t*)n;
  local->kind = EXPR_PARAM;
  local->name = name;
  local->init = expr(p, PREC_COMMA, fl);
  local->type = local->init->type;
  bubble_flags(local, local->init);
  return (expr_t*)n;
}


// args = ( arg (("," | ";") arg) ("," | ";")? )?
// arg  = expr | id ":" expr
static void args(parser_t* p, call_t* n, type_t* recvtype, nodeflag_t fl) {
  local_t param0 = { {{EXPR_PARAM}}, .type = recvtype };
  local_t** paramv = (local_t*[]){ &param0 };
  u32 paramc = 1;

  if (recvtype->kind == TYPE_FUN) {
    funtype_t* ft = (funtype_t*)recvtype;
    paramv = (local_t**)ft->params.v;
    paramc = ft->params.len;
    if (paramc > 0 && paramv[0]->isthis) {
      paramv++;
      paramc--;
    }
  } else if (recvtype->kind == TYPE_STRUCT) {
    structtype_t* st = (structtype_t*)recvtype;
    paramv = (local_t**)st->fields.v;
    paramc = st->fields.len;
  }

  fl |= NF_RVALUE;

  for (u32 paramidx = 0; ;paramidx++) {
    // type_t* t = (paramidx < paramc) ? paramv[paramidx]->type : type_void;

    expr_t* arg;
    if (currtok(p) == TID) {
      // name:value
      arg = named_param_or_id(p, fl);
    } else {
      // value
      arg = expr(p, PREC_COMMA, fl);
    }

    push_child(p, &n->args, arg);
    bubble_flags(n, arg);

    if (currtok(p) != TSEMI && currtok(p) != TCOMMA)
      return;
    next(p);
  }

}


static expr_t* prim_typecons(parser_t* p, type_t* t, nodeflag_t fl) {
  assert((t->flags & NF_UNKNOWN) == 0);
  typecons_t* n = mkexpr(p, typecons_t, EXPR_TYPECONS, fl);
  next(p); // consume "("
  n->type = t;

  if (currtok(p) != TRPAREN) {
    n->expr = expr(p, PREC_COMMA, fl | NF_RVALUE);
    bubble_flags(n, n->expr);
  }

  if LIKELY(currtok(p) == TRPAREN) {
    next(p);
    return (expr_t*)n;
  }

  // error
  // e.g. "int(1,2)"
  if (currtok(p) == TCOMMA) {
    scanstate_t scanstate = save_scanstate(p);
    next(p);
    if (currtok(p) == TRPAREN || currtok(p) == TSEMI) {
      restore_scanstate(p, scanstate);
      expect_fail(p, TRPAREN, "to end type cast");
    } else {
      error(p, NULL, "unexpected extra value in type cast");
    }
  } else {
    expect_fail(p, TRPAREN, "to end type cast");
  }
  fastforward_semi(p);
  return (expr_t*)n;
}


// call = expr "(" args? ")"
static expr_t* expr_postfix_call(
  parser_t* p, const parselet_t* pl, expr_t* recv, nodeflag_t fl)
{
  type_t* recvtype = recv->type;

  // common case of primitive typecast
  idexpr_t* id = (idexpr_t*)recv;
  if (id->kind == EXPR_ID && id->ref && nodekind_isprimtype(id->ref->kind))
    return prim_typecons(p, (type_t*)id->ref, fl);

  call_t* n = mkexpr(p, call_t, EXPR_CALL, fl);
  next(p);
  n->recv = recv;
  recv->flags |= NF_RVALUE;
  bubble_flags(n, recv);

  if (recv->type && recv->type->kind == TYPE_FUN) {
    funtype_t* ft = (funtype_t*)recv->type;
    n->type = ft->result;
  } else if (recv->type && nodekind_istype(recvtype->kind)) {
    n->type = recv->type;
    recvtype = recv->type;
  } else {
    error(p, n, "calling %s; expected function or type",
      recv->type ? nodekind_fmt(recv->type->kind) : nodekind_fmt(recv->kind));
  }

  // args?
  if (currtok(p) != TRPAREN)
    args(p, n, recvtype, fl);
  n->argsendloc = currloc(p);
  expect(p, TRPAREN, "to end function call");

  return (expr_t*)n;
}


// subscript = expr "[" expr "]"
static expr_t* expr_postfix_subscript(
  parser_t* p, const parselet_t* pl, expr_t* left, nodeflag_t fl)
{
  unaryop_t* n = mkexpr(p, unaryop_t, EXPR_POSTFIXOP, fl);
  next(p);
  left->flags |= NF_RVALUE;
  bubble_flags(n, left);
  panic("TODO");
  return (expr_t*)n;
}


// member = expr "." id
static expr_t* expr_postfix_member(
  parser_t* p, const parselet_t* pl, expr_t* left, nodeflag_t fl)
{
  member_t* n = mkexpr(p, member_t, EXPR_MEMBER, fl);
  next(p);
  left->flags |= NF_RVALUE * (nodeflag_t)(left != p->dotctx);
  n->recv = left;
  n->name = p->scanner.sym;
  bubble_flags(n, n->recv);
  expect(p, TID, "");
  return (expr_t*)n;
}


// dotmember = "." id
static expr_t* expr_dotmember(parser_t* p, const parselet_t* pl, nodeflag_t fl) {
  if UNLIKELY(!p->dotctx) {
    error(p, NULL, "\"this\" selector in regular function");
    expr_t* n = mkbad(p);
    fastforward_semi(p);
    return n;
  }
  return expr_postfix_member(p, pl, p->dotctx, fl);
}


static bool funtype_params(parser_t* p, funtype_t* ft, type_t* nullable recvt) {
  // params = "(" param (sep param)* sep? ")"
  // param  = Id Type? | Type
  // sep    = "," | ";"
  //
  // e.g.  (T)  (x T)  (x, y T)  (T1, T2, T3)

  bool isnametype = false; // when at least one param has type; e.g. "x T"

  // typeq: temporary storage for params to support "typed groups" of parameters,
  // e.g. "x, y int" -- "x" does not have a type until we parsed "y" and "int", so when
  // we parse "x" we put it in typeq. Also, "x" might be just a type and not a name in
  // the case all args are just types e.g. "T1, T2, T3".
  ptrarray_t typeq = {0}; // local_t*[]

  while (currtok(p) != TEOF) {
    local_t* param = mkexpr(p, local_t, EXPR_PARAM, 0);
    if UNLIKELY(param == NULL)
      goto oom;
    param->type = NULL; // clear type_void set by mkexpr for later check

    if (!ptrarray_push(&ft->params, p->ast_ma, param))
      goto oom;

    // "mut this"?
    bool this_ismut = false;
    if (currtok(p) == TMUT && ft->params.len == 1 && lookahead_issym(p, sym_this)) {
      this_ismut = true;
      next(p);
    }

    if (currtok(p) == TID) {
      // name, eg "x"; could be parameter name or type. Assume name for now.
      param->name = p->scanner.sym;
      param->loc = currloc(p);
      next(p);

      // check for "this" as first argument
      if (param->name == sym_this && ft->params.len == 1 && recvt) {
        isnametype = true;
        param->isthis = true;
        param->ismut = this_ismut;
        param->type = recvt;
        // param->type = this_type(p, recvt, this_ismut);
        bubble_flags(param, param->type);
        // note: intentionally NOT bubble_flags to ft or param
        goto loopend;
      }

      switch (currtok(p)) {
        case TRPAREN:
        case TCOMMA:
        case TSEMI: // just a name, eg "x" in "(x, y)"
          if (!ptrarray_push(&typeq, p->ma, param))
            goto oom;
          break;
        default: // type follows name, eg "int" in "x int"
          param->type = type(p, PREC_LOWEST);
          bubble_flags(param, param->type);
          isnametype = true;
          // cascade type to predecessors
          for (u32 i = 0; i < typeq.len; i++) {
            local_t* prev_param = typeq.v[i];
            prev_param->type = param->type;
            bubble_flags(prev_param, param->type);
          }
          typeq.len = 0;
      }

    } else {
      // definitely a type
      param->name = sym__;
      if (!param->name)
        goto oom;
      param->type = type(p, PREC_LOWEST);
      bubble_flags(param, param->type);
    }

  loopend:
    bubble_flags(ft, param);
    switch (currtok(p)) {
      case TCOMMA:
      case TSEMI:
        next(p); // consume "," or ";"
        if (currtok(p) == TRPAREN) {
          // trailing "," or ";"
          // e.g.
          //   fun foo(
          //     a int <implicit ";">
          //     b int <implicit ";">  <— trailing semicolon
          //   )
          // e.g.
          //   fun foo(
          //     a int,
          //     b int,  <— trailing comma
          //   )
          goto finish;
        }
        break; // continue reading more
      case TRPAREN:
        goto finish;
      default:
        unexpected(p, "expecting ',' ';' or ')'");
        fastforward(p, (const tok_t[]){ TRPAREN, 0 });
        goto error;
    }
  } // while(!EOF)
finish:
  if (isnametype) {
    // name-and-type form; e.g. "(x, y T, z Y)".
    // Error if at least one param has type, but last one doesn't, e.g. "(x, y int, z)"
    if UNLIKELY(typeq.len > 0) {
      error(p, NULL, "expecting type");
      for (u32 i = 0; i < ft->params.len; i++) {
        local_t* param = (local_t*)ft->params.v[i];
        if (!param->type)
          param->type = type_void;
        bubble_flags(param, param->type);
      }
    }
  } else {
    // type-only form, e.g. "(T, T, Y)"
    for (u32 i = 0; i < ft->params.len; i++) {
      local_t* param = (local_t*)ft->params.v[i];
      if (param->type)
        continue;
      // make type from id
      param->type = named_type(p, param->name, param->nameloc);
      param->name = sym__;
      bubble_flags(param, param->type);
    }
  }
  ptrarray_dispose(&typeq, p->ma);
  if (isnametype)
    ft->flags |= NF_NAMEDPARAMS;
  return true;
oom:
  out_of_mem(p);
error:
  ptrarray_dispose(&typeq, p->ma);
  return false;
}


static funtype_t* funtype(parser_t* p, loc_t loc, type_t* nullable recvt) {
  funtype_t* ft = mknode(p, funtype_t, TYPE_FUN);
  ft->loc = loc;
  ft->size = p->scanner.compiler->ptrsize;
  ft->align = p->scanner.compiler->ptrsize;
  ft->result = type_void;

  // parameters
  ft->paramsloc = currloc(p);
  if UNLIKELY(!expect(p, TLPAREN, "for parameters")) {
    fastforward(p, (const tok_t[]){ TLBRACE, TSEMI, 0 });
    return ft;
  }
  if (currtok(p) != TRPAREN)
    funtype_params(p, ft, recvt);
  ft->paramsendloc = currloc(p);
  expect(p, TRPAREN, "to match '('");

  // result type
  // no result type implies "void", e.g. "fun foo() {}" => "fun foo() void {}"
  if (currtok(p) != TLBRACE) {
    ft->resultloc = currloc(p);
    ft->result = type(p, PREC_MEMBER);
    if (loc_line(ft->result->loc) && ft->result->loc > ft->loc)
      ft->resultloc = ft->result->loc;
    bubble_flags(ft, ft->result);
  }

  if ((ft->flags & NF_UNKNOWN) == 0)
    intern_usertype(p->scanner.compiler, (usertype_t**)&ft);

  return ft;
}


static type_t* type_fun(parser_t* p) {
  loc_t loc = currloc(p);
  next(p); // consume TFUN
  return (type_t*)funtype(p, loc, /*recvt*/NULL);
}


static map_t* nullable get_or_create_recvtmap(parser_t* p, const type_t* t) {
  // get or create function map for type
  void** mmp = map_assign_ptr(&p->recvtmap, p->ma, t);
  if UNLIKELY(!mmp)
    return out_of_mem(p), NULL;
  if (!*mmp) {
    if (!(*mmp = mem_alloct(p->ma, map_t)) || !map_init(*mmp, p->ma, 8))
      return out_of_mem(p), NULL;
  }
  return *mmp;
}


static void typefun_add(parser_t* p, type_t* recvt, sym_t name, fun_t* fun, loc_t loc) {
  assertnotnull(fun->name);
  assert(fun->recvt == NULL || fun->recvt == recvt);
  assert(name != sym__);

  if (fun->name == sym__)
    fun->name = name;

  fun->recvt = recvt;

  map_t* mm = get_or_create_recvtmap(p, recvt);
  if UNLIKELY(!mm)
    return;
  void** mp = map_assign_ptr(mm, p->ma, name);
  if UNLIKELY(!mp)
    return out_of_mem(p);

  expr_t* existing = *mp;
  if (!existing && recvt->kind == TYPE_STRUCT)
    existing = (expr_t*)lookup_struct_field((structtype_t*)recvt, name);

  if UNLIKELY(existing) {
    const char* s = fmtnode(p, 0, recvt);
    error(p, loc, "duplicate %s \"%s\" for type %s",
      (existing->kind == EXPR_FUN) ? "function" : "member", name, s);
    if (loc_line(existing->loc))
      help(p, existing, "\"%s\" previously defined here", name);
    return;
  }

  *mp = fun;
}


static void fun_body(parser_t* p, fun_t* n, nodeflag_t fl) {
  funtype_t* ft = (funtype_t*)n->type;

  if (funtype_hasthis(ft)) {
    assert(ft->params.len > 0);
    local_t* thisparam = ft->params.v[0];
    assert(thisparam->kind == EXPR_PARAM);

    // idexpr_t* id = mkexpr(p, idexpr_t, EXPR_ID, fl);
    // id->loc = 0;
    // id->name = thisparam->name;
    // id->type = thisparam->type;
    // id->ref = (node_t*)thisparam;
    // dotctx_push(p, (expr_t*)id);

    dotctx_push(p, (expr_t*)thisparam);
  }

  fun_t* outer_fun = p->fun;
  p->fun = n;

  fl |= NF_RVALUE;
  if (ft->result == type_void)
    fl &= ~NF_RVALUE;

  enter_scope(p);

  n->body = any_as_block(p, fl);

  // even though it may have implicit return, in practice a function body
  // block is never an expression itself.
  n->body->flags &= ~NF_RVALUE;
  bubble_flags(n, n->body);

  leave_scope(p);

  p->fun = outer_fun;

  if (funtype_hasthis(ft))
    dotctx_pop(p);
}


// fundef = "fun" name "(" params? ")" result ( ";" | "{" body "}")
// result = params
// body   = (stmt ";")*
static fun_t* fun(parser_t* p, nodeflag_t fl, type_t* nullable recvt, bool requirename) {
  fun_t* n = mkexpr(p, fun_t, EXPR_FUN, fl);
  next(p);
  n->recvt = recvt;

  if (p->fun) {
    // nested functions are only visible within the same source file
    n->visibility = VISIBILITY_PRIVATE;
  }

  // name
  if (currtok(p) == TID) {
    n->name = p->scanner.sym;
    n->nameloc = currloc(p);
    next(p);
    if (n->recvt == NULL && currtok(p) == TDOT) {
      // type function, e.g. "Foo.bar"
      next(p); // consume "."
      sym_t recv_name = n->name; // e.g. "Foo" in "Foo.bar"
      loc_t recv_nameloc = n->nameloc;
      n->name = p->scanner.sym;
      n->nameloc = currloc(p);
      expect(p, TID, "");

      // Require that type functions be defined after the receiver type is defined.
      // Allowing forward definition of types here would be confusing in practice.
      node_t* t = lookup(p, recv_name);
      if LIKELY(t && node_istype(t)) {
        n->recvt = (type_t*)t;
      } else {
        n->recvt = mkunresolvedtype(p, recv_name, recv_nameloc);
        if (!t) {
          error(p, recv_nameloc, "no such type \"%s\"", recv_name);
        } else {
          error(p, recv_nameloc, "%s is not a type", recv_name);
        }
      }
    }
    if (n->recvt)
      typefun_add(p, n->recvt, n->name, n, n->nameloc);
  } else if (requirename) {
    error(p, NULL, "missing function name");
  }

  // parameters and result type
  n->type = (type_t*)funtype(p, n->loc, n->recvt);
  bubble_flags(n, n->type);

  // define named function
  if (n->name && n->type->kind != NODE_BAD && !n->recvt)
    define(p, n->name, (node_t*)n);

  // no body?
  if (currtok(p) == TSEMI)
    return n;

  // define named parameters
  funtype_t* ft = (funtype_t*)n->type;
  if (ft->flags & NF_NAMEDPARAMS) {
    enter_scope(p);
    for (u32 i = 0; i < ft->params.len; i++)
      define(p, ((local_t*)ft->params.v[i])->name, ft->params.v[i]);
  } else if UNLIKELY(ft->params.len > 0) {
    error(p, NULL, "function without named parameters can't have a body");
    help(p, ft->paramsloc, "name parameter%s \"_\"", ft->params.len > 1 ? "s" : "");
  }

  fun_body(p, n, fl);

  if (ft->flags & NF_NAMEDPARAMS)
    leave_scope(p);

  return n;
}

static expr_t* expr_fun(parser_t* p, const parselet_t* pl, nodeflag_t fl) {
  return (expr_t*)fun(p, fl, /*recvt*/NULL, /*requirename*/false);
}

static stmt_t* stmt_fun(parser_t* p) {
  return (stmt_t*)fun(p, 0, /*recvt*/NULL, /*requirename*/true);
}


static stmt_t* stmt_pub(parser_t* p) {
  loc_t pub_loc = currloc(p);
  next(p);

  // pub "ABI"
  abi_t abi = ABI_CO;
  if (currtok(p) == TSTRLIT) {
    slice_t str = scanner_strval(&p->scanner);
    if (slice_eq_cstri(str, "C")) {
      abi = ABI_C;
    } else if (slice_eq_cstri(str, "CO")) {
      // abi is already "CO"
    } else {
      buf_t* buf = tmpbuf(0);
      if (!buf_appendrepr(buf, str.bytes, str.len)) {
        out_of_mem(p);
      } else {
        error(p, pub_loc, "invalid ABI: \"%.*s\"; expected \"C\" or \"co\"",
          (int)buf->len, buf->chars);
      }
    }
    next(p);
  }

  stmt_t* n = stmt(p);
  switch (n->kind) {
    case EXPR_FUN:
      ((fun_t*)n)->visibility = VISIBILITY_PUBLIC;
      ((fun_t*)n)->abi = abi;
      break;
    case STMT_TYPEDEF:
      ((typedef_t*)n)->visibility = VISIBILITY_PUBLIC;
      break;
    case NODE_BAD:
      break;
    default:
      error(p, pub_loc, "unexpected pub qualifier on %s", nodekind_fmt(n->kind));
  }

  return n;
}


unit_t* parser_parse(parser_t* p, memalloc_t ast_ma, input_t* input) {
  p->ast_ma = ast_ma;
  scope_clear(&p->scope);
  scanner_set_input(&p->scanner, input);
  unit_t* unit = mknode(p, unit_t, NODE_UNIT);
  p->unit = unit;
  next(p);

  enter_scope(p);

  while (currtok(p) != TEOF) {
    void** np = ptrarray_alloc(&unit->children, p->ast_ma, 1);
    if UNLIKELY(!np) {
      out_of_mem(p);
      break;
    }

    *np = stmt(p);
    bubble_flags(unit, *np);

    if UNLIKELY(currtok(p) != TSEMI) {
      if (currtok(p) != TEOF)
        expect_fail(p, TSEMI, "");
      break;
    }

    next(p); // consume ";"
  }

  leave_scope(p);

  p->unit = NULL;

  return unit;
}


static map_t* nullable compiler_builtins(compiler_t* c) {
  if (c->builtins.cap > 0)
    return &c->builtins;

  const struct {
    sym_t       sym;
    const void* node;
  } entries[] = {
    // types
    {sym_cstr("void"), type_void},
    {sym_cstr("bool"), type_bool},
    {sym_cstr("int"),  type_int},
    {sym_cstr("uint"), type_uint},
    {sym_cstr("i8"),   type_i8},
    {sym_cstr("i16"),  type_i16},
    {sym_cstr("i32"),  type_i32},
    {sym_cstr("i64"),  type_i64},
    {sym_cstr("u8"),   type_u8},
    {sym_cstr("u16"),  type_u16},
    {sym_cstr("u32"),  type_u32},
    {sym_cstr("u64"),  type_u64},
    {sym_cstr("f32"),  type_f32},
    {sym_cstr("f64"),  type_f64},
    {sym_str,          &c->strtype},
  };

  if UNLIKELY(!map_init(&c->builtins, c->ma, countof(entries)))
    return NULL;

  for (usize i = 0; i < countof(entries); i++) {
    void** valp = map_assign_ptr(&c->builtins, c->ma, entries[i].sym);
    assertnotnull(valp);
    *valp = (void*)entries[i].node;
  }
  return &c->builtins;
}


bool parser_init(parser_t* p, compiler_t* c) {
  memset(p, 0, sizeof(*p));

  map_t* builtins = compiler_builtins(c);
  if (!builtins)
    return false;

  if (!scanner_init(&p->scanner, c))
    return false;

  if (!map_init(&p->pkgdefs, c->ma, 32))
    goto err1;
  p->pkgdefs.parent = builtins;

  if (!map_init(&p->tmpmap, c->ma, 32))
    goto err2;
  if (!map_init(&p->recvtmap, c->ma, 32))
    goto err3;

  p->ma = p->scanner.compiler->ma;

  // note: dotctxstack is valid when zero initialized
  p->dotctx = NULL;

  return true;
err3:
  map_dispose(&p->tmpmap, c->ma);
err2:
  map_dispose(&p->pkgdefs, c->ma);
err1:
  scanner_dispose(&p->scanner);
  return false;
}


void parser_dispose(parser_t* p) {
  map_dispose(&p->pkgdefs, p->ma);
  map_dispose(&p->tmpmap, p->ma);
  map_dispose(&p->recvtmap, p->ma);
  ptrarray_dispose(&p->dotctxstack, p->ma);
  scanner_dispose(&p->scanner);
}


// parselet tables


static const parselet_t expr_parsetab[TOK_COUNT] = {
  // infix ops (in order of precedence from weakest to strongest)
  [TASSIGN]    = {NULL, expr_assign_op,          PREC_ASSIGN, OP_ASSIGN},     // =
  [TADDASSIGN] = {NULL, expr_assign_op,          PREC_ASSIGN, OP_ADD_ASSIGN}, // +=
  [TSUBASSIGN] = {NULL, expr_assign_op,          PREC_ASSIGN, OP_SUB_ASSIGN}, // -=
  [TMULASSIGN] = {NULL, expr_assign_op,          PREC_ASSIGN, OP_MUL_ASSIGN}, // *=
  [TDIVASSIGN] = {NULL, expr_assign_op,          PREC_ASSIGN, OP_DIV_ASSIGN}, // /=
  [TMODASSIGN] = {NULL, expr_assign_op,          PREC_ASSIGN, OP_MOD_ASSIGN}, // %=
  [TSHLASSIGN] = {NULL, expr_assign_op,          PREC_ASSIGN, OP_SHL_ASSIGN}, // <<=
  [TSHRASSIGN] = {NULL, expr_assign_op,          PREC_ASSIGN, OP_SHR_ASSIGN}, // >>=
  [TANDASSIGN] = {NULL, expr_assign_op,          PREC_ASSIGN, OP_AND_ASSIGN}, // &=
  [TORASSIGN]  = {NULL, expr_assign_op,          PREC_ASSIGN, OP_OR_ASSIGN},  // |=
  [TXORASSIGN] = {NULL, expr_assign_op,          PREC_ASSIGN, OP_XOR_ASSIGN}, // ^=
  [TOROR]      = {NULL, expr_cmp_op,             PREC_LOGICAL_OR, OP_LOR},    // ||
  [TANDAND]    = {NULL, expr_cmp_op,             PREC_LOGICAL_AND, OP_LAND},  // &&
  [TAND]       = {expr_ref, expr_infix_op,       PREC_BITWISE_AND, OP_AND},   // &
  [TOR]        = {NULL, expr_infix_op,           PREC_BITWISE_OR, OP_OR},     // |
  [TXOR]       = {NULL, expr_infix_op,           PREC_BITWISE_XOR, OP_XOR},   // ^
  [TEQ]        = {NULL, expr_cmp_op,             PREC_EQUAL, OP_EQ},          // ==
  [TNEQ]       = {NULL, expr_cmp_op,             PREC_EQUAL, OP_NEQ},         // !=
  [TLT]        = {NULL, expr_cmp_op,             PREC_COMPARE, OP_LT},        // <
  [TGT]        = {NULL, expr_cmp_op,             PREC_COMPARE, OP_GT},        // >
  [TLTEQ]      = {NULL, expr_cmp_op,             PREC_COMPARE, OP_LTEQ},      // <=
  [TGTEQ]      = {NULL, expr_cmp_op,             PREC_COMPARE, OP_GTEQ},      // >=
  [TSHL]       = {NULL, expr_infix_op,           PREC_SHIFT, OP_SHL},         // <<
  [TSHR]       = {NULL, expr_infix_op,           PREC_SHIFT, OP_SHR},         // >>
  [TPLUS]      = {expr_prefix_op, expr_infix_op, PREC_ADD, OP_ADD},           // +
  [TMINUS]     = {expr_prefix_op, expr_infix_op, PREC_ADD, OP_SUB},           // -
  [TSTAR]      = {expr_deref, expr_infix_op,     PREC_MUL, OP_MUL},           // *
  [TSLASH]     = {NULL, expr_infix_op,           PREC_MUL, OP_DIV},           // /
  [TPERCENT]   = {NULL, expr_infix_op,           PREC_MUL, OP_MOD},           // %

  // prefix and postfix ops (in addition to the ones above)
  [TPLUSPLUS]   = {expr_prefix_op, expr_postfix_op, PREC_UNARY_PREFIX, OP_INC}, // ++
  [TMINUSMINUS] = {expr_prefix_op, expr_postfix_op, PREC_UNARY_PREFIX, OP_DEC}, // --
  [TNOT]        = {expr_prefix_op, NULL,            0, OP_NOT},                 // !
  [TTILDE]      = {expr_prefix_op, NULL,            0, OP_INV},                 // ~

  // constants
  [TINTLIT]   = {expr_intlit},
  [TFLOATLIT] = {expr_floatlit},
  [TSTRLIT]   = {expr_strlit},
  [TTRUE]     = {expr_boollit},
  [TFALSE]    = {expr_boollit},

  // punctuation, keywords, identifiers etc
  [TLPAREN] = {expr_group, expr_postfix_call, PREC_UNARY_POSTFIX},         // (
  [TLBRACK] = {expr_arraylit, expr_postfix_subscript, PREC_UNARY_POSTFIX}, // [
  [TLBRACE] = {expr_block},                                                // {
  [TDOT]    = {expr_dotmember, expr_postfix_member, PREC_MEMBER},          // .
  [TMUT]    = {expr_mut},
  [TID]     = {expr_id},
  [TFUN]    = {expr_fun},
  [TLET]    = {expr_var},
  [TVAR]    = {expr_var},
  [TIF]     = {expr_if},
  [TFOR]    = {expr_for},
  [TRETURN] = {expr_return},
};


// type
static const type_parselet_t type_parsetab[TOK_COUNT] = {
  [TID]       = {type_id, NULL, 0},       // T
  [TLBRACK]   = {type_array, NULL, 0},    // [T N], [T]
  [TLBRACE]   = {type_struct, NULL, 0},   // {...}
  [TFUN]      = {type_fun, NULL, 0},      // fun(T)T
  [TSTAR]     = {type_ptr, NULL, 0},      // *T
  [TAND]      = {type_ref, NULL, 0},      // &T
  [TMUT]      = {type_mut, NULL, 0},      // mut&T
  [TQUESTION] = {type_optional, NULL, 0}, // ?T
};


// statement
static const stmt_parselet_t stmt_parsetab[TOK_COUNT] = {
  [TPUB]  = {stmt_pub, NULL, 0},
  [TFUN]  = {stmt_fun, NULL, 0},
  [TTYPE] = {stmt_typedef, NULL, 0},
};
