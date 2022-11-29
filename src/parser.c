#include "c0lib.h"
#include "compiler.h"
#include "abuf.h"

#include <stdlib.h>

// #define LOG_PRATT(fmt, args...) log("parse> " fmt, ##args)
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
  PREC_UNARY_PREFIX,  // ++  --  +  -  !  ~  *  &  ?
  PREC_UNARY_POSTFIX, // ++  --  ()  []
  PREC_MEMBER,        // .

  PREC_LOWEST = PREC_COMMA,
} precedence_t;


//#define PPARAMS parser_t* p, precedence_t prec
#define PARGS   p, prec

typedef stmt_t*(*prefix_stmt_parselet_t)(parser_t* p);
typedef stmt_t*(*infix_stmt_parselet_t)(parser_t* p, precedence_t prec, stmt_t* left);

typedef expr_t*(*prefix_expr_parselet_t)(parser_t* p);
typedef expr_t*(*infix_expr_parselet_t)(parser_t* p, precedence_t prec, expr_t* left);

typedef type_t*(*prefix_type_parselet_t)(parser_t* p);
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
static struct { node_t; u8 opaque[64]; } _last_resort_node = { .kind=NODE_BAD };
node_t* last_resort_node = (node_t*)&_last_resort_node;


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


inline static scanstate_t save_scanstate(parser_t* p) {
  return *(scanstate_t*)&p->scanner;
}

inline static void restore_scanstate(parser_t* p, scanstate_t state) {
  *(scanstate_t*)&p->scanner = state;
}


inline static tok_t currtok(parser_t* p) {
  return p->scanner.tok.t;
}

inline static srcloc_t currloc(parser_t* p) {
  return p->scanner.tok.loc;
}


static void next(parser_t* p) {
  scanner_next(&p->scanner);
}


static tok_t lookahead(parser_t* p, u32 distance) {
  scanstate_t scanstate = save_scanstate(p);
  while (distance--)
    next(p);
  tok_t tok = currtok(p);
  restore_scanstate(p, scanstate);
  return tok;
}


static bool lookahead_issym(parser_t* p, sym_t sym) {
  scanstate_t scanstate = save_scanstate(p);
  next(p);
  bool ok = currtok(p) == TID && p->scanner.sym == sym;
  restore_scanstate(p, scanstate);
  return ok;
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


// node_srcrange computes the source range for an AST
srcrange_t node_srcrange(const node_t* n) {
  srcrange_t r = { .start = n->loc, .focus = n->loc };
  switch (n->kind) {
    case EXPR_INTLIT:
      r.end.line = r.focus.line;
      r.end.col = r.focus.col + u64log10( ((intlit_t*)n)->intval);
      break;
    case EXPR_ID:
      r.end.line = r.focus.line;
      r.end.col = r.focus.col + strlen(((idexpr_t*)n)->name);
  }
  return r;
}


ATTR_FORMAT(printf,3,4)
static void error(parser_t* p, const node_t* nullable n, const char* fmt, ...) {
  if (p->scanner.inp == p->scanner.inend && p->scanner.tok.t == TEOF)
    return;
  srcrange_t srcrange = n ? node_srcrange(n) : (srcrange_t){ .focus = currloc(p), };
  va_list ap;
  va_start(ap, fmt);
  report_diagv(p->scanner.compiler, srcrange, DIAG_ERR, fmt, ap);
  va_end(ap);
}


ATTR_FORMAT(printf,3,4)
static void warning(parser_t* p, const node_t* nullable n, const char* fmt, ...) {
  srcrange_t srcrange = n ? node_srcrange(n) : (srcrange_t){ .focus = currloc(p), };
  va_list ap;
  va_start(ap, fmt);
  report_diagv(p->scanner.compiler, srcrange, DIAG_WARN, fmt, ap);
  va_end(ap);
}


static void out_of_mem(parser_t* p) {
  error(p, NULL, "out of memory");
  // end scanner, making sure we don't keep going
  p->scanner.inp = p->scanner.inend;
}


static const char* fmttok(parser_t* p, usize bufindex, tok_t tok, slice_t lit) {
  buf_t* buf = &p->tmpbuf[bufindex];
  buf_clear(buf);
  buf_reserve(buf, 64);
  tok_descr(buf->p, buf->cap, tok, lit);
  return buf->chars;
}


static const char* fmtnode(parser_t* p, u32 bufindex, const node_t* n, u32 depth) {
  buf_t* buf = &p->tmpbuf[bufindex];
  buf_clear(buf);
  node_fmt(buf, n, depth);
  return buf->chars;
}


static void unexpected(parser_t* p, const char* errmsg) {
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


static void enter_scope(parser_t* p) {
  if (!scope_push(&p->scope, p->scanner.compiler->ma))
    out_of_mem(p);
}


static void leave_scope(parser_t* p) {
  // check for unused definitions
  for (u32 i = p->scope.base + 1; i < p->scope.len; i++) {
    const node_t* n = p->scope.ptr[i++];
    sym_t name = p->scope.ptr[i];
    if (name != sym__ && node_isexpr(n) && ((const expr_t*)n)->nrefs == 0 &&
        n->kind != EXPR_FUN && (n->kind != EXPR_PARAM || !((local_t*)n)->isthis) )
    {
      warning(p, n, "unused %s \"%s\"", nodekind_fmt(n->kind), name);
    }
  }
  scope_pop(&p->scope);
}


static node_t* nullable lookup_definition(parser_t* p, sym_t name) {
  node_t* n = scope_lookup(&p->scope, name, U32_MAX);
  if (!n) {
    // look in package scope and its parent universe scope
    void** vp = map_lookup(&p->pkgdefs, name, strlen(name));
    if (!vp)
      return NULL;
    n = *vp;
  }
  // increase reference count
  if (node_isexpr(n)) {
    ((expr_t*)n)->nrefs++;
  } else if (node_isusertype(n)) {
    ((usertype_t*)n)->nrefs++;
  }
  return n;
}


static void define_replace(parser_t* p, sym_t name, node_t* n) {
  assert(name != sym__);
  if (!scope_def(&p->scope, p->scanner.compiler->ma, name, n))
    out_of_mem(p);
  if (scope_istoplevel(&p->scope)) {
    void** vp = map_assign(&p->pkgdefs, p->scanner.compiler->ma, name, strlen(name));
    if (!vp)
      return out_of_mem(p);
    *vp = n;
  }
}


static void define(parser_t* p, sym_t name, node_t* n) {
  //dlog("define %s => %s@%p", name, nodekind_name(n->kind), n);
  if (name == sym__)
    return;

  node_t* existing = scope_lookup(&p->scope, name, 0);
  if (existing)
    goto err_duplicate;

  if (!scope_def(&p->scope, p->scanner.compiler->ma, name, n))
    out_of_mem(p);

  // top-level definitions also goes into package scope
  if (scope_istoplevel(&p->scope)) {
    void** vp = map_assign(&p->pkgdefs, p->scanner.compiler->ma, name, strlen(name));
    if (!vp)
      return out_of_mem(p);
    if (*vp)
      goto err_duplicate;
    *vp = n;
  }
  return;
err_duplicate:
  error(p, n, "redefinition of \"%s\"", name);
}


#define mknode(p, TYPE, kind)  ( (TYPE*)_mknode((p), sizeof(TYPE), (kind)) )
#define mkexpr(p, TYPE, kind)  ( (TYPE*)_mkexpr((p), sizeof(TYPE), (kind)) )

static node_t* _mknode(parser_t* p, usize size, nodekind_t kind) {
  mem_t m = mem_alloc_zeroed(p->ast_ma, size);
  if UNLIKELY(m.p == NULL)
    return out_of_mem(p), last_resort_node;
  node_t* n = m.p;
  n->kind = kind;
  n->loc = currloc(p);
  return n;
}


static expr_t* _mkexpr(parser_t* p, usize size, nodekind_t kind) {
  assertf(nodekind_isexpr(kind), "%s", nodekind_name(kind));
  expr_t* n = (expr_t*)_mknode(p, size, kind);
  if ((node_t*)n != last_resort_node)
    n->type = type_void;
  return n;
}


static void* mkbad(parser_t* p) {
  return mknode(p, __typeof__(_last_resort_node), NODE_BAD);
}


static reftype_t* mkreftype(parser_t* p, bool ismut) {
  reftype_t* t = mknode(p, reftype_t, TYPE_REF);
  t->size = p->scanner.compiler->ptrsize;
  t->align = t->size;
  t->ismut = ismut;
  return t;
}


static void push(parser_t* p, ptrarray_t* children, void* child) {
  if UNLIKELY(!ptrarray_push(children, p->ast_ma, child))
    out_of_mem(p);
}


static void typectx_push(parser_t* p, type_t* t) {
  if UNLIKELY(!ptrarray_push(&p->typectxstack, p->scanner.compiler->ma, p->typectx))
    out_of_mem(p);
  p->typectx = t;
}

static void typectx_pop(parser_t* p) {
  assert(p->typectxstack.len > 0);
  p->typectx = ptrarray_pop(&p->typectxstack);
}


static void dotctx_push(parser_t* p, expr_t* nullable n) {
  if UNLIKELY(!ptrarray_push(&p->dotctxstack, p->scanner.compiler->ma, p->dotctx))
    out_of_mem(p);
  p->dotctx = n;
}

static void dotctx_pop(parser_t* p) {
  assert(p->dotctxstack.len > 0);
  p->dotctx = ptrarray_pop(&p->dotctxstack);
}


static bool types_isconvertible(const type_t* dst, const type_t* src) {
  assertnotnull(dst);
  assertnotnull(src);
  if (dst == src)
    return true;
  if (type_isprim(dst) && type_isprim(src))
    return true;
  return false;
}


static bool types_iscompat(const type_t* dst, const type_t* src) {
  assertnotnull(dst);
  assertnotnull(src);
  switch (dst->kind) {
    case TYPE_INT:
    case TYPE_I8:
    case TYPE_I16:
    case TYPE_I32:
    case TYPE_I64:
      return (dst == src) && (dst->isunsigned == src->isunsigned);
    case TYPE_REF: {
      // &T    <= &T
      // mut&T <= &T
      // mut&T <= mut&T
      // &T    x= mut&T
      if (src->kind != TYPE_REF)
        return false;
      const reftype_t* d = (const reftype_t*)dst;
      const reftype_t* s = (const reftype_t*)src;
      if (types_iscompat(d->elem, s->elem) &&
          (s->ismut == d->ismut || s->ismut || !d->ismut))
      {
        return true;
      }
      return false;
    }
    case TYPE_OPTIONAL: {
      // ?T <= T
      // ?T <= ?T
      const opttype_t* d = (const opttype_t*)dst;
      if (src->kind == TYPE_OPTIONAL)
        src = ((const opttype_t*)src)->elem;
      return types_iscompat(d->elem, src);
    }
  }
  return dst == src;
}


static void check_types_compat(
  parser_t* p,
  const type_t* nullable x,
  const type_t* nullable y,
  const node_t* nullable origin)
{
  if UNLIKELY(!!x * !!y && !types_iscompat(x, y)) { // "!!x * !!y": ignore NULL
    const char* xs = fmtnode(p, 0, (const node_t*)x, 1);
    const char* ys = fmtnode(p, 1, (const node_t*)y, 1);
    error(p, origin, "incompatible types, %s and %s", xs, ys);
  }
}


static stmt_t* stmt(parser_t* p, precedence_t prec) {
  tok_t tok = currtok(p);
  const stmt_parselet_t* parselet = &stmt_parsetab[tok];
  log_pratt(p, "prefix stmt");
  if UNLIKELY(!parselet->prefix) {
    unexpected(p, "where a statement is expected");
    fastforward_semi(p);
    return mkbad(p);
  }
  stmt_t* n = parselet->prefix(p);
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
  expr_t* n = parselet->prefix(p);
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
    unexpected(p, "where type is expected");
    fastforward_semi(p);
    return type_void;
  }
  type_t* t = parselet->prefix(p);
  for (;;) {
    tok = currtok(p);
    parselet = &type_parsetab[tok];
    log_pratt_infix(p, "type", parselet->infix, parselet->prec, prec);
    if (parselet->infix == NULL || parselet->prec < prec)
      return t;
    t = parselet->infix(PARGS, t);
  }
}


static type_t* named_type(parser_t* p, sym_t name, const node_t* nullable origin) {
  const node_t* ref = lookup_definition(p, name);
  if UNLIKELY(!ref) {
    error(p, origin, "unknown type \"%s\"", name);
  } else if UNLIKELY(!node_istype(ref)) {
    error(p, origin, "%s is not a type", name);
  } else {
    return (type_t*)ref;
  }
  return type_void;
}


static type_t* type_id(parser_t* p) {
  type_t* t = named_type(p, p->scanner.sym, NULL);
  next(p);
  return t;
}


static local_t* nullable find_field(ptrarray_t* fields, sym_t name) {
  for (u32 i = 0; i < fields->len; i++) {
    local_t* f = fields->v[i];
    if (f->name == name)
      return f;
  }
  return NULL;
}


static fun_t* nullable find_methodv(ptrarray_t* methods, sym_t name) {
  for (u32 i = 0; i < methods->len; i++) {
    fun_t* f = methods->v[i];
    if (f->name == name)
      return f;
  }
  return NULL;
}


static fun_t* nullable find_method(parser_t* p, type_t* t, sym_t name) {
  if (t->kind == TYPE_STRUCT) {
    fun_t* f = find_methodv(&((structtype_t*)t)->methods, name);
    if (f)
      return f;
  }
  void** mmp = map_lookup_ptr(&p->methodmap, t);
  if (!mmp)
    return NULL;
  map_t* mm = assertnotnull(*mmp);
  void** mp = map_lookup_ptr(mm, name);
  if (!mp)
    return NULL;
  return assertnotnull(*mp);
}


// field = id ("," id)* type ("=" expr ("," expr))
static bool fieldset(parser_t* p, ptrarray_t* fields) {
  u32 fields_start = fields->len;
  for (;;) {
    local_t* f = mknode(p, local_t, NODE_FIELD);
    f->name = p->scanner.sym;
    if (find_field(fields, f->name))
      error(p, NULL, "duplicate field %s", f->name);
    // TODO: check for own methods with find_methodv
    expect(p, TID, "");
    push(p, fields, f);
    if (currtok(p) != TCOMMA)
      break;
    next(p);
  }

  type_t* t = type(p, PREC_MEMBER);
  for (u32 i = fields_start; i < fields->len; i++)
    ((local_t*)fields->v[i])->type = t;

  if (currtok(p) != TASSIGN)
    return false;

  next(p);
  u32 i = fields_start;
  for (;;) {
    if (i == fields->len) {
      error(p, NULL, "excess field initializer");
      expr(p, PREC_COMMA);
      break;
    }
    local_t* f = fields->v[i++];
    typectx_push(p, f->type);
    f->init = expr(p, PREC_COMMA);
    typectx_pop(p);
    if (currtok(p) != TCOMMA)
      break;
    next(p);
    if UNLIKELY(!types_iscompat(f->type, f->init->type)) {
      const char* got = fmtnode(p, 0, (const node_t*)f->init->type, 1);
      const char* expect = fmtnode(p, 1, (const node_t*)f->type, 1);
      error(p, (node_t*)f->init,
        "field initializer of type %s where type %s is expected", got, expect);
    }
  }
  if (i < fields->len)
    error(p, NULL, "missing field initializer");
  return true;
}


static type_t* type_struct(parser_t* p) {
  structtype_t* t = mknode(p, structtype_t, TYPE_STRUCT);
  next(p);
  while (currtok(p) != TRBRACE) {
    t->hasinit |= fieldset(p, &t->fields);
    if (currtok(p) != TSEMI)
      break;
    next(p);
  }
  expect(p, TRBRACE, "to end struct");
  for (u32 i = 0; i < t->fields.len; i++) {
    local_t* f = t->fields.v[i];
    type_t* ft = assertnotnull(f->type);
    t->align = MAX(t->align, ft->align);
    t->size += ft->size;
  }
  t->size = ALIGN2(t->size, t->align);
  //dlog("struct size %zu, align %u", t->size, t->align);
  return (type_t*)t;
}


static type_t* type_ref1(parser_t* p, bool ismut) {
  reftype_t* t = mkreftype(p, ismut);
  next(p);
  t->elem = type(p, PREC_UNARY_PREFIX);
  return (type_t*)t;
}


// ref_type = "&" type
static type_t* type_ref(parser_t* p) {
  return type_ref1(p, /*ismut*/false);
}


// mut_type = "mut" ref_type
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
  return (type_t*)t;
}


// typedef = "type" id type
static stmt_t* stmt_typedef(parser_t* p) {
  typedef_t* n = mknode(p, typedef_t, STMT_TYPEDEF);
  next(p);
  n->name = p->scanner.sym;
  bool nameok = expect(p, TID, "");
  if (nameok)
    define(p, n->name, (node_t*)n);
  n->type = type(p, PREC_COMMA);
  if (nameok && !scope_def(&p->scope, p->scanner.compiler->ma, n->name, n->type))
    out_of_mem(p);
  if (n->type->kind == TYPE_STRUCT)
    ((structtype_t*)n->type)->name = n->name;
  return (stmt_t*)n;
}


static idexpr_t* resolve_id(parser_t* p, idexpr_t* n) {
  n->ref = lookup_definition(p, n->name);
  if UNLIKELY(!n->ref) {
    error(p, (node_t*)n, "undeclared identifier \"%s\"", n->name);
  } else if (node_isexpr(n->ref)) {
    n->type = ((expr_t*)n->ref)->type;
  } else if (nodekind_istype(n->ref->kind)) {
    n->type = (type_t*)n->ref;
  } else {
    error(p, (node_t*)n, "cannot use %s \"%s\" as an expression",
      nodekind_fmt(n->ref->kind), n->name);
  }
  return n;
}


static expr_t* expr_id(parser_t* p) {
  idexpr_t* n = mkexpr(p, idexpr_t, EXPR_ID);
  n->name = p->scanner.sym;
  next(p);
  return (expr_t*)resolve_id(p, n);
}


static expr_t* expr_var(parser_t* p) {
  local_t* n = mkexpr(p, local_t, currtok(p) == TLET ? EXPR_LET : EXPR_VAR);
  next(p);
  if (currtok(p) != TID) {
    unexpected(p, "expecting identifier");
    return mkbad(p);
  } else {
    n->name = p->scanner.sym;
    next(p);
  }
  if (currtok(p) == TASSIGN) {
    next(p);
    typectx_push(p, type_void);
    n->init = expr(p, PREC_ASSIGN);
    typectx_pop(p);
    n->type = n->init->type;
  } else {
    n->type = type(p, PREC_LOWEST);
    if (currtok(p) == TASSIGN) {
      next(p);
      typectx_push(p, n->type);
      n->init = expr(p, PREC_ASSIGN);
      typectx_pop(p);
      check_types_compat(p, n->type, n->init->type, (node_t*)n->init);
    }
  }
  define(p, n->name, (node_t*)n);
  return (expr_t*)n;
}


// T* CLONE_NODE(T* node)
#define CLONE_NODE(nptr) ( \
  (__typeof__(nptr))memcpy( \
    mkexpr(p, __typeof__(*(nptr)), ((node_t*)(nptr))->kind), \
    (nptr), \
    sizeof(*(nptr))) \
)


static void check_if_cond(parser_t* p, expr_t* cond) {
  if (cond->type->kind == TYPE_BOOL)
    return;
  if (type_isopt(cond->type)) {
    // redefine as non-optional
    if (cond->kind == EXPR_ID) {
      // e.g. "if x { ... }"
      idexpr_t* v1 = (idexpr_t*)cond;
      idexpr_t* v2 = CLONE_NODE(v1);
      v2->type = ((opttype_t*)v2->type)->elem;
      define_replace(p, v2->name, (node_t*)v2);
      return;
    }
    if (cond->kind == EXPR_LET || cond->kind == EXPR_VAR) {
      // e.g. "if let x = expr { ... }"
      // note that we must copy the local even though it is only used within
      // the "then" branch to retain the information about the optional check.
      local_t* v1 = (local_t*)cond;
      local_t* v2 = CLONE_NODE(v1);
      v2->type = ((opttype_t*)v2->type)->elem;
      define_replace(p, v2->name, (node_t*)v2);
      return;
    }
    dlog("TODO if-check on optional of kind %s", nodekind_name(cond->kind));
  }
  error(p, (node_t*)cond, "conditional is not a boolean");
}


static expr_t* expr_if(parser_t* p) {
  ifexpr_t* n = mkexpr(p, ifexpr_t, EXPR_IF);
  next(p);

  enter_scope(p);

  n->cond = expr(p, PREC_COMMA);
  check_if_cond(p, n->cond);
  n->thenb = expr(p, PREC_COMMA);
  if (currtok(p) == TELSE) {
    next(p);
    n->elseb = expr(p, PREC_COMMA);
  }

  // n->type = n->thenb; // TODO: type of if...else (rvalue flags needed?)

  leave_scope(p);

  return (expr_t*)n;
}


// for       = "for" ( for_head | for_phead ) expr
// for_head  = ( expr | expr? ";" expr ";" expr? )
// for_phead = "(" for_head ")"
static expr_t* expr_for(parser_t* p) {
  forexpr_t* n = mkexpr(p, forexpr_t, EXPR_FOR);
  next(p);
  bool paren = currtok(p) == TLPAREN;
  if (paren)
    next(p);
  if (currtok(p) == TSEMI) {
    // "for ; i < 4; i++"
    next(p);
    n->cond = expr(p, PREC_COMMA);
    expect(p, TSEMI, "");
    next(p);
    n->end = expr(p, PREC_COMMA);
  } else {
    // "for i < 4"
    n->cond = expr(p, PREC_COMMA);
    if (currtok(p) == TSEMI) {
      // "for i = 0; i < 4; i++"
      next(p);
      n->start = n->cond;
      n->cond = expr(p, PREC_COMMA);
      expect(p, TSEMI, "");
      n->end = expr(p, PREC_COMMA);
    }
  }
  if (paren)
    expect(p, TRPAREN, "");
  n->body = expr(p, PREC_COMMA);
  return (expr_t*)n;
}


// return = "return" (expr ("," expr)*)?
static expr_t* expr_return(parser_t* p) {
  retexpr_t* n = mkexpr(p, retexpr_t, EXPR_RETURN);
  next(p);
  if (currtok(p) == TSEMI)
    return (expr_t*)n;
  for (;;) {
    expr_t* value = expr(p, PREC_COMMA);
    push(p, &n->values, value);
    if (currtok(p) != TCOMMA)
      break;
    next(p);
  }
  if (n->values.len == 1) {
    n->type = ((expr_t*)n->values.v[0])->type;
  } else {
    dlog("TODO tuple type");
  }
  return (expr_t*)n;
}


static type_t* select_int_type(parser_t* p, const intlit_t* n, u64 isneg) {
  type_t* type = p->typectx;
  u64 maxval = 0;
  u64 uintval = n->intval;
  if (isneg)
    uintval &= ~0x1000000000000000; // clear negative bit

  bool u = type->isunsigned;

  switch (type->kind) {
  case TYPE_I8:  maxval = u ? 0xffllu               : 0x7fllu+isneg; break;
  case TYPE_I16: maxval = u ? 0xffffllu             : 0x7fffllu+isneg; break;
  case TYPE_I32: maxval = u ? 0xffffffffllu         : 0x7fffffffllu+isneg; break;
  case TYPE_I64: maxval = u ? 0xffffffffffffffffllu : 0x7fffffffffffffffllu+isneg; break;
  default: // all other type contexts results in TYPE_INT
    if (isneg) {
      if (uintval <= 0x80000000llu)         return type_int;
      if (uintval <= 0x8000000000000000llu) return type_i64;
      // trigger error report
      maxval = 0x8000000000000000llu;
      type = type_i64;
    } else {
      if (n->intval <= 0x7fffffffllu)         return type_int;
      if (n->intval <= 0x7fffffffffffffffllu) return type_i64;
      maxval = 0xffffffffffffffffllu;
      type = type_u64;
    }
  }

  if UNLIKELY(uintval > maxval) {
    const char* ts = fmtnode(p, 0, (const node_t*)type, 1);
    slice_t lit = scanner_lit(&p->scanner);
    error(p, (node_t*)n, "integer constant %s%.*s overflows %s",
      isneg ? "-" : "", (int)lit.len, lit.chars, ts);
  }
  return type;
}


static expr_t* intlit(parser_t* p, bool isneg) {
  intlit_t* n = mkexpr(p, intlit_t, EXPR_INTLIT);
  n->intval = p->scanner.litint;
  n->type = select_int_type(p, n, (u64)isneg);
  next(p);
  return (expr_t*)n;
}


static expr_t* floatlit(parser_t* p, bool isneg) {
  floatlit_t* n = mkexpr(p, floatlit_t, EXPR_FLOATLIT);
  char* endptr = NULL;

  // note: scanner always starts float litbuf with '+'
  if (isneg)
    p->scanner.litbuf.chars[0] = '-';

  if (p->typectx == type_f32) {
    n->type = type_f32;
    n->f32val = strtof(p->scanner.litbuf.chars, &endptr);
    if (endptr != p->scanner.litbuf.chars + p->scanner.litbuf.len) {
      error(p, (node_t*)n, "invalid floating-point constant");
    } else if (n->f32val == HUGE_VALF) {
      error(p, (node_t*)n, "32-bit floating-point constant too large");
    }
  } else {
    n->type = type_f64;
    n->f64val = strtod(p->scanner.litbuf.chars, &endptr);
    if (endptr != p->scanner.litbuf.chars + p->scanner.litbuf.len) {
      error(p, (node_t*)n, "invalid floating-point constant");
    } else if (n->f64val == HUGE_VAL) {
      // e.g. 1.e999
      error(p, (node_t*)n, "64-bit floating-point constant too large");
    }
  }

  next(p);
  return (expr_t*)n;
}


static expr_t* expr_intlit(parser_t* p) {
  return intlit(p, /*isneg*/false);
}


static expr_t* expr_floatlit(parser_t* p) {
  return floatlit(p, /*isneg*/false);
}


static expr_t* expr_prefix_op(parser_t* p) {
  unaryop_t* n = mkexpr(p, unaryop_t, EXPR_PREFIXOP);
  n->op = currtok(p);
  next(p);
  switch (currtok(p)) {
    // special case for negative number constants
    case TINTLIT:   n->expr = intlit(p, /*isneg*/n->op == TMINUS); break;
    case TFLOATLIT: n->expr = floatlit(p, /*isneg*/n->op == TMINUS); break;
    default:        n->expr = expr(p, PREC_UNARY_PREFIX);
  }
  n->type = n->expr->type;
  return (expr_t*)n;
}


static expr_t* expr_infix_op(parser_t* p, precedence_t prec, expr_t* left) {
  binop_t* n = mkexpr(p, binop_t, EXPR_BINOP);
  n->op = currtok(p);
  next(p);

  n->type = left->type;
  n->left = left;

  typectx_push(p, n->type);
  n->right = expr(p, prec);
  typectx_pop(p);

  check_types_compat(p, n->left->type, n->right->type, (node_t*)n);
  return (expr_t*)n;
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


static void check_assign_to_member(parser_t* p, member_t* m) {
  // check mutability of receiver
  assertnotnull(m->recv->type);
  switch (m->recv->type->kind) {

  case TYPE_STRUCT:
    // assignment to non-ref "this", e.g. "fun Foo.bar(this Foo) { this = Foo() }"
    if UNLIKELY(
      m->recv->kind == EXPR_ID &&
      ((idexpr_t*)m->recv)->ref->kind == EXPR_PARAM &&
      ((local_t*)((idexpr_t*)m->recv)->ref)->isthis)
    {
      const char* s = fmtnode(p, 0, (node_t*)m->recv, 1);
      error(p, (node_t*)m->recv, "assignment to immutable struct %s", s);
      return;
    }
    break;

  case TYPE_REF:
    if (!((reftype_t*)m->recv->type)->ismut) {
      const char* s = fmtnode(p, 0, (node_t*)m->recv, 1);
      error(p, (node_t*)m->recv, "assignment to immutable reference %s", s);
      return;
    }
    break;

  }
}


static void check_assign_to_id(parser_t* p, idexpr_t* id) {
  node_t* target = id->ref; // target is NULL when "id" is undefined
  if (target) switch (target->kind) {
  case EXPR_VAR:
    break;
  case EXPR_PARAM:
    if (!((local_t*)target)->isthis)
      break;
    FALLTHROUGH;
  default:
    error(p, (node_t*)id, "cannot assign to %s \"%s\"",
      nodekind_fmt(target->kind), id->name);
  }
}


static void check_assign(parser_t* p, expr_t* target) {
  switch (target->kind) {
  case EXPR_ID:
    return check_assign_to_id(p, (idexpr_t*)target);
  case EXPR_MEMBER:
    return check_assign_to_member(p, (member_t*)target);
  case EXPR_DEREF: {
    // dereference target, e.g. "var x &int ; *x = 3"
    type_t* t = ((unaryop_t*)target)->expr->type;
    if (t->kind != TYPE_REF)
      goto err;
    if UNLIKELY(!((reftype_t*)t)->ismut) {
      const char* s = fmtnode(p, 0, (node_t*)t, 1);
      error(p, (node_t*)target, "cannot assign via immutable reference of type %s", s);
    }
    return;
  }
  }
err:
  error(p, (node_t*)target, "cannot assign to %s", nodekind_fmt(target->kind));
}


static expr_t* expr_infix_assign(parser_t* p, precedence_t prec, expr_t* left) {
  binop_t* n = (binop_t*)expr_infix_op(p, prec, left);
  check_assign(p, n->left);
  return (expr_t*)n;
}


static expr_t* postfix_op(parser_t* p, precedence_t prec, expr_t* left) {
  unaryop_t* n = mkexpr(p, unaryop_t, EXPR_POSTFIXOP);
  n->op = currtok(p);
  next(p);
  n->expr = left;
  n->type = left->type;
  check_assign(p, left);
  return (expr_t*)n;
}


// deref_expr = "*" expr
static expr_t* expr_deref(parser_t* p) {
  unaryop_t* n = mkexpr(p, unaryop_t, EXPR_DEREF);
  n->op = currtok(p);
  next(p);
  n->expr = expr(p, PREC_UNARY_PREFIX);
  reftype_t* t = (reftype_t*)n->expr->type;

  if UNLIKELY(t->kind != TYPE_REF) {
    const char* ts = fmtnode(p, 0, (node_t*)t, 1);
    error(p, (node_t*)n, "dereferencing non-reference value of type %s", ts);
  } else {
    n->type = t->elem;
  }

  return (expr_t*)n;
}


// ref_expr = "&" location
static expr_t* expr_ref1(parser_t* p, bool ismut) {
  unaryop_t* n = mkexpr(p, unaryop_t, EXPR_PREFIXOP);
  n->op = currtok(p);
  next(p);
  n->expr = expr(p, PREC_UNARY_PREFIX);

  if UNLIKELY(n->expr->type->kind == TYPE_REF) {
    const char* ts = fmtnode(p, 0, (node_t*)n->expr->type, 1);
    error(p, (node_t*)n, "referencing reference type %s", ts);
  } else if UNLIKELY(!expr_isstorage(n->expr)) {
    const char* ts = fmtnode(p, 0, (node_t*)n->expr->type, 1);
    error(p, (node_t*)n, "referencing ephemeral value of type %s", ts);
  } else if UNLIKELY(ismut && !expr_ismut(n->expr)) {
    const char* s = fmtnode(p, 0, (node_t*)n->expr, 1);
    nodekind_t k = n->expr->kind;
    if (k == EXPR_ID)
      k = ((idexpr_t*)n->expr)->ref->kind;
    error(p, (node_t*)n, "mutable reference to immutable %s %s", nodekind_fmt(k), s);
  }

  reftype_t* t = mkreftype(p, ismut);
  t->elem = n->expr->type;
  n->type = (type_t*)t;
  return (expr_t*)n;
}

static expr_t* expr_ref(parser_t* p) {
  return expr_ref1(p, /*ismut*/false);
}


// mut_expr = "mut" ref_expr
static expr_t* expr_mut(parser_t* p) {
  next(p);
  if UNLIKELY(currtok(p) != TAND) {
    unexpected(p, "expecting '&'");
    return mkbad(p);
  }
  return expr_ref1(p, /*ismut*/true);
}


// group = "(" expr ")"
static expr_t* expr_group(parser_t* p) {
  next(p);
  expr_t* n = expr(p, PREC_COMMA);
  expect(p, TRPAREN, "");
  return n;
}


static void error_field_type(parser_t* p, const expr_t* arg, const local_t* f) {
  const char* got = fmtnode(p, 0, (const node_t*)arg->type, 1);
  const char* expect = fmtnode(p, 1, (const node_t*)f->type, 1);
  const node_t* origin = (const node_t*)arg;
  if (arg->kind == EXPR_PARAM)
    origin = assertnotnull((const node_t*)((local_t*)arg)->init);
  error(p, origin, "passing value of type %s for field \"%s\" of type %s",
    got, f->name, expect);
}


static void validate_structcall_args(parser_t* p, call_t* call) {
  const structtype_t* t = (const structtype_t*)call->recv->type;
  assert(call->args.len <= t->fields.len); // checked by validate_typecall_args

  u32 i = 0;

  // positional arguments
  for (; i < call->args.len; i++) {
    const expr_t* arg = call->args.v[i];
    if (arg->kind == EXPR_PARAM)
      break;
    const local_t* f = t->fields.v[i];
    if UNLIKELY(!types_iscompat(f->type, arg->type))
      error_field_type(p, arg, f);
  }

  if (i == t->fields.len)
    return;

  // named arguments
  u32 posend = i;
  map_t* seen = &p->tmpmap;
  map_clear(seen);

  for (u32 i = 0; i < t->fields.len; i++) {
    const local_t* f = t->fields.v[i];
    void** vp = map_assign_ptr(seen, p->scanner.compiler->ma, f->name);
    if UNLIKELY(!vp)
      return out_of_mem(p);
    if (i < posend) {
      *vp = call->args.v[i];
    } else {
      *vp = (void*)f;
    }
  }

  for (; i < call->args.len; i++) {
    const local_t* arg = call->args.v[i];
    assert(arg->kind == EXPR_PARAM); // checked by namedargs
    const void** vp = (const void**)map_lookup_ptr(seen, arg->name);
    if UNLIKELY(!vp || ((const node_t*)*vp)->kind == EXPR_PARAM) {
      const char* s = fmtnode(p, 0, (const node_t*)t, 1);
      if (!vp) {
        error(p, (node_t*)arg, "unknown field \"%s\" in struct %s", arg->name, s);
      } else {
        error(p, (node_t*)arg, "duplicate value for field \"%s\" in struct %s",
          arg->name, s);
        warning(p, *vp, "value for field \"%s\" already provided here", arg->name);
      }
      continue;
    }

    const local_t* f = *vp;
    *vp = arg;

    if UNLIKELY(!types_iscompat(f->type, arg->type))
      error_field_type(p, (const expr_t*)arg, f);
  }
}


static void validate_primtypecall_arg(parser_t* p, call_t* call) {
  const type_t* dst = call->recv->type;
  assert(call->args.len == 1); // checked by validate_typecall_args
  const expr_t* arg = call->args.v[0];
  if UNLIKELY(!nodekind_isexpr(arg->kind))
    return error(p, (node_t*)arg, "invalid value");
  const type_t* src = arg->type;
  if UNLIKELY(dst != src && !types_isconvertible(dst, src)) {
    const char* dst_s = fmtnode(p, 0, (node_t*)dst, 1);
    const char* src_s = fmtnode(p, 1, (node_t*)src, 1);
    error(p, (node_t*)arg, "cannot convert value of type %s to type %s", src_s, dst_s);
  }
}


static void validate_typecall_args(parser_t* p, call_t* call) {
  const type_t* t = (const type_t*)call->recv->type;
  u32 minargs = 0;
  u32 maxargs = 0;

  switch (t->kind) {
  case TYPE_VOID:
    break;
  case TYPE_BOOL:
  case TYPE_INT:
  case TYPE_I8:
  case TYPE_I16:
  case TYPE_I32:
  case TYPE_I64:
  case TYPE_F32:
  case TYPE_F64:
    minargs = 1;
    maxargs = 1;
    break;
  case TYPE_STRUCT:
    maxargs = ((const structtype_t*)t)->fields.len;
    break;
  case TYPE_ARRAY:
    minargs = 1;
    maxargs = U32_MAX;
    FALLTHROUGH;
  case TYPE_ENUM:
  case TYPE_REF:
    dlog("NOT IMPLEMENTED: %s", nodekind_name(t->kind));
    error(p, (node_t*)call->recv, "NOT IMPLEMENTED: %s", nodekind_name(t->kind));
    break;
  default:
    assertf(0,"unexpected %s", nodekind_name(t->kind));
  }

  if UNLIKELY(call->args.len < minargs) {
    const node_t* origin = (const node_t*)call->recv;
    if (call->args.len > 0)
      origin = call->args.v[call->args.len - 1];
    const char* typ = fmtnode(p, 0, (const node_t*)t, 1);
    return error(p, origin,
      "not enough arguments for %s type constructor, expecting%s %u",
      typ, minargs != maxargs ? " at least" : "", minargs);
  }

  if UNLIKELY(call->args.len > maxargs) {
    const node_t* arg = call->args.v[maxargs];
    const char* argstr = fmtnode(p, 0, arg, 1);
    const char* typstr = fmtnode(p, 1, (const node_t*)t, 1);
    if (maxargs == 0) {
      return error(p, arg, "unexpected value %s; %s type accepts no arguments",
        argstr, typstr);
    }
    return error(p, arg, "unexpected extra value %s in %s type constructor",
      argstr, typstr);
  }

  if (nodekind_isprimtype(t->kind))
    return validate_primtypecall_arg(p, call);

  if (t->kind == TYPE_STRUCT)
    return validate_structcall_args(p, call);
}


static void validate_funcall_args(parser_t* p, call_t* call) {
  const funtype_t* ft = (const funtype_t*)call->recv->type;

  u32 paramsc = ft->params.len;
  local_t** paramsv = (local_t**)ft->params.v;
  if (paramsc > 0 && paramsv[0]->isthis) {
    paramsv++;
    paramsc--;
  }

  if UNLIKELY(call->args.len != paramsc) {
    return error(p, (const node_t*)call,
      "%s arguments in function call, expected %u",
      call->args.len < paramsc ? "not enough" : "too many", paramsc);
  }

  for (u32 i = 0; i < paramsc; i++) {
    expr_t* arg = call->args.v[i];
    local_t* param = paramsv[i];
    // check name
    if UNLIKELY(arg->kind == EXPR_PARAM && ((local_t*)arg)->name != param->name) {
      for (i = 0; i < paramsc; i++) {
        if (paramsv[i]->name == ((local_t*)arg)->name)
          break;
      }
      const char* fts = fmtnode(p, 0, (const node_t*)ft, 1);
      return error(p, (node_t*)arg,
        "%s named argument \"%s\", in function call %s",
        i == paramsc ? "unknown" : "invalid position for",
        ((local_t*)arg)->name, fts);
    }
    // check type
    if UNLIKELY(!types_iscompat(param->type, arg->type)) {
      const char* got = fmtnode(p, 0, (const node_t*)arg->type, 1);
      const char* expect = fmtnode(p, 1, (const node_t*)param->type, 1);
      error(p, (node_t*)arg, "passing %s to parameter of type %s", got, expect);
    }
  }
}


static void validate_call_args(parser_t* p, call_t* call) {
  if (call->recv->type->kind == TYPE_FUN)
    return validate_funcall_args(p, call);
  assert(nodekind_istype(call->recv->type->kind));
  return validate_typecall_args(p, call);
}


// namedargs = id ":" expr ("," id ":" expr)*
static void namedargs(parser_t* p, ptrarray_t* args, local_t** paramv, u32 paramc) {
  for (u32 paramidx = 0; ;paramidx++) {
    local_t* namedarg = mkexpr(p, local_t, EXPR_PARAM);
    namedarg->name = p->scanner.sym;
    if (currtok(p) != TID) {
      unexpected(p, ", expecting field name");
      break;
    }
    next(p);
    if (currtok(p) != TCOLON) {
      unexpected(p, ", expecting ':' after field name");
      break;
    }
    next(p);
    if (paramidx < paramc)
      typectx_push(p, paramv[paramidx]->type);
    namedarg->init = expr(p, PREC_COMMA);
    if (paramidx < paramc)
      typectx_pop(p);
    namedarg->type = namedarg->init->type;
    push(p, args, namedarg);
    if (currtok(p) != TSEMI && currtok(p) != TCOMMA)
      break;
    next(p);
  }
}


// args      = posargs ("," namedargs)
//           | namedargs
// posargs   = expr ("," expr)*
static void args(parser_t* p, ptrarray_t* args, type_t* recvtype) {
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

  typectx_push(p, type_void);

  for (u32 paramidx = 0; ;paramidx++) {
    if (currtok(p) == TID && lookahead(p, 1) == TCOLON) {
      if (paramidx >= paramc) {
        paramc = 0;
      } else {
        paramv += paramidx;
        paramc -= paramidx;
      }
      return namedargs(p, args, paramv, paramc);
    }

    if (paramidx < paramc)
      typectx_push(p, paramv[paramidx]->type);
    expr_t* arg = expr(p, PREC_COMMA);
    if (paramidx < paramc)
      typectx_pop(p);

    push(p, args, arg);

    if (currtok(p) != TSEMI && currtok(p) != TCOMMA)
      return;
    next(p);
  }

  typectx_pop(p);
}


// call = expr "(" args? ")"
static expr_t* expr_postfix_call(parser_t* p, precedence_t prec, expr_t* left) {
  u32 errcount = p->scanner.compiler->errcount;
  call_t* n = mkexpr(p, call_t, EXPR_CALL);
  next(p);
  type_t* recvtype = left->type;
  if (left->type && left->type->kind == TYPE_FUN) {
    funtype_t* ft = (funtype_t*)left->type;
    n->type = ft->result;
  } else if (left->type && nodekind_istype(left->type->kind)) {
    n->type = left->type;
    recvtype = left->type;
  } else {
    error(p, (node_t*)n, "calling %s; expected function or type",
      left->type ? nodekind_fmt(left->type->kind) : nodekind_fmt(left->kind));
  }
  n->recv = left;
  if (currtok(p) != TRPAREN)
    args(p, &n->args, recvtype ? recvtype : type_void);
  if (errcount == p->scanner.compiler->errcount)
    validate_call_args(p, n);
  expect(p, TRPAREN, "to end function call");
  return (expr_t*)n;
}


// subscript = expr "[" expr "]"
static expr_t* expr_postfix_subscript(parser_t* p, precedence_t prec, expr_t* left) {
  unaryop_t* n = mkexpr(p, unaryop_t, EXPR_POSTFIXOP);
  next(p);
  panic("TODO");
  return (expr_t*)n;
}


// member = expr "." id
static expr_t* expr_postfix_member(parser_t* p, precedence_t prec, expr_t* left) {
  member_t* n = mkexpr(p, member_t, EXPR_MEMBER);
  next(p);
  n->recv = left;
  n->name = p->scanner.sym;
  if (!expect(p, TID, ""))
    return (expr_t*)n;

  // get struct type, unwrapping optional and ref
  structtype_t* st = assertnotnull((structtype_t*)n->recv->type);
  if (st->kind == TYPE_OPTIONAL) {
    opttype_t* opt = (opttype_t*)st;
    st = assertnotnull((structtype_t*)opt->elem);
  }
  if (st->kind == TYPE_REF) {
    reftype_t* pt = (reftype_t*)st;
    st = assertnotnull((structtype_t*)pt->elem);
  }

  if UNLIKELY(st->kind != TYPE_STRUCT) {
    const char* s = fmtnode(p, 0, (const node_t*)st, 1);
    error(p, (node_t*)n, "%s has no member \"%s\"", s, n->name);
    return (expr_t*)n;
  }

  // search for field
  for (local_t* f = find_field(&st->fields, n->name); f; ) {
    n->target = (expr_t*)f;
    n->type = f->type;
    return (expr_t*)n;
  }

  // search for method
  for (fun_t* f = find_method(p, (type_t*)st, n->name); f; ) {
    n->target = (expr_t*)f;
    n->type = f->type;
    return (expr_t*)n;
  }

  const char* s = fmtnode(p, 0, (const node_t*)n->recv, 1);
  error(p, (node_t*)n, "%s has no field \"%s\"", s, n->name);
  return (expr_t*)n;
}


// dotmember = "." id
static expr_t* expr_dotmember(parser_t* p) {
  if UNLIKELY(!p->dotctx) {
    error(p, NULL, "\".\" shorthand outside of context");
    expr_t* n = mkbad(p);
    fastforward_semi(p);
    return n;
  }
  return expr_postfix_member(p, PREC_MEMBER, p->dotctx);
}


static expr_t* expr_block(parser_t* p) {
  block_t* n = mkexpr(p, block_t, EXPR_BLOCK);
  next(p);
  enter_scope(p);
  while (currtok(p) != TRBRACE && currtok(p) != TEOF) {
    push(p, &n->children, (node_t*)expr(p, PREC_LOWEST));
    if (currtok(p) != TSEMI)
      break;
    next(p);
  }
  expect2(p, TRBRACE, ", expected '}' or ';'");
  leave_scope(p);
  if (n->children.len > 0) {
    expr_t* last_expr = n->children.v[n->children.len-1];
    n->type = last_expr->type;
  }
  return (expr_t*)n;
}


static type_t* this_param_type(parser_t* p, type_t* recvt, bool ismut) {
  if (!ismut) {
    // pass certain types as value instead of pointer when access is read-only
    if (nodekind_isprimtype(recvt->kind)) // e.g. int, i32
      return recvt;
    if (recvt->kind == TYPE_STRUCT) {
      // small structs
      structtype_t* st = (structtype_t*)recvt;
      usize ptrsize = p->scanner.compiler->ptrsize;
      if (st->align <= ptrsize && st->size <= ptrsize*2)
        return recvt;
    }
  }
  // pointer type
  reftype_t* t = mkreftype(p, ismut);
  t->elem = recvt;
  return (type_t*)t;
}


static void this_param(parser_t* p, fun_t* fun, local_t* param, bool ismut) {
  if UNLIKELY(!fun->methodof) {
    param->type = type_void;
    param->nrefs = 1; // prevent "unused parameter" warning
    error(p, (node_t*)param, "\"this\" parameter of non-method function");
    return;
  }
  param->isthis = true;
  param->type = this_param_type(p, fun->methodof, ismut);
}


static bool fun_params(parser_t* p, fun_t* fun) {
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
    local_t* param = mkexpr(p, local_t, EXPR_PARAM);
    if UNLIKELY(param == NULL)
      goto oom;
    param->type = NULL; // clear type_void set by mkexpr for later check

    if (!ptrarray_push(&fun->params, p->ast_ma, param))
      goto oom;

    bool this_ismut = false;
    if (currtok(p) == TMUT && fun->params.len == 1 && lookahead_issym(p, sym_this)) {
      this_ismut = true;
      next(p);
    }

    if (currtok(p) == TID) {
      // name, eg "x"; could be parameter name or type. Assume name for now.
      param->name = p->scanner.sym;
      param->loc = currloc(p);
      next(p);

      // check for "this" as first argument
      if (param->name == sym_this && fun->params.len == 1) {
        isnametype = true;
        this_param(p, fun, param, this_ismut);
        goto loopend;
      }

      switch (currtok(p)) {
        case TRPAREN:
        case TCOMMA:
        case TSEMI: // just a name, eg "x" in "(x, y)"
          if (!ptrarray_push(&typeq, p->ast_ma, param))
            goto oom;
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
        goto oom;
      param->type = type(p, PREC_LOWEST);
    }

  loopend:
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
        fastforward(p, (const tok_t[]){ TRPAREN, TSEMI, 0 });
        goto finish;
    }
  }
finish:
  if (isnametype) {
    // name-and-type form; e.g. "(x, y T, z Y)".
    // Error if at least one param has type, but last one doesn't, e.g. "(x, y int, z)"
    if UNLIKELY(typeq.len > 0) {
      error(p, NULL, "expecting type");
      for (u32 i = 0; i < fun->params.len; i++) {
        local_t* param = (local_t*)fun->params.v[i];
        if (!param->type)
          param->type = type_void;
      }
    }
  } else {
    // type-only form, e.g. "(T, T, Y)"
    for (u32 i = 0; i < fun->params.len; i++) {
      local_t* param = (local_t*)fun->params.v[i];
      if (param->type)
        continue;
      // make type from id
      param->type = named_type(p, param->name, (node_t*)param);
      param->name = sym__;
    }
  }
  ptrarray_dispose(&typeq, p->ast_ma);
  return isnametype;
oom:
  out_of_mem(p);
  return false;
}


// T** typeidmap_assign(parser_t*, sym_t tid, T, nodekind_t)
#define typeidmap_assign(p, tid, T, kind) \
  (T**)_typeidmap_assign((p), (tid), (kind))
static type_t** _typeidmap_assign(parser_t* p, sym_t tid, nodekind_t kind) {
  compiler_t* c = p->scanner.compiler;
  type_t** tp = (type_t**)map_assign_ptr(&c->typeidmap, c->ma, tid);
  if UNLIKELY(!tp) {
    out_of_mem(p);
    return (type_t**)&last_resort_node;
  }
  if (*tp)
    assert((*tp)->kind == kind);
  return tp;
}


static sym_t typeid_fun(parser_t* p, const ptrarray_t* params, type_t* result) {
  buf_t* buf = &p->tmpbuf[0];
  buf_clear(buf);
  buf_push(buf, TYPEID_PREFIX(TYPE_FUN));
  if UNLIKELY(!buf_print_leb128_u32(buf, params->len))
    goto fail;
  for (u32 i = 0; i < params->len; i++) {
    local_t* param = params->v[i];
    assert(param->kind == EXPR_PARAM);
    if UNLIKELY(!typeid_append(buf, assertnotnull(param->type)))
      goto fail;
  }
  if UNLIKELY(!typeid_append(buf, result))
    goto fail;
  return sym_intern(buf->p, buf->len);
fail:
  out_of_mem(p);
  return sym__;
}


static funtype_t* funtype(parser_t* p, ptrarray_t* params, type_t* result) {
  // build typeid
  sym_t tid = typeid_fun(p, params, result);

  // find existing function type
  funtype_t** typeidmap_slot = typeidmap_assign(p, tid, funtype_t, TYPE_FUN);
  if (*typeidmap_slot)
    return *typeidmap_slot;

  // build function type
  funtype_t* ft = mknode(p, funtype_t, TYPE_FUN);
  ft->size = p->scanner.compiler->ptrsize;
  ft->align = ft->size;
  ft->isunsigned = true;
  ft->result = result;
  if UNLIKELY(!ptrarray_reserve(&ft->params, p->ast_ma, params->len)) {
    out_of_mem(p);
  } else {
    ft->params.len = params->len;
    for (u32 i = 0; i < params->len; i++) {
      local_t* param = params->v[i];
      assert(param->kind == EXPR_PARAM);
      ft->params.v[i] = param;
    }
  }
  *typeidmap_slot = ft;
  return ft;
}


static map_t* nullable get_or_create_methodmap(parser_t* p, const type_t* t) {
  // get or create method map for type
  memalloc_t ma = p->scanner.compiler->ma;
  void** mmp = map_assign_ptr(&p->methodmap, ma, t);
  if UNLIKELY(!mmp)
    return out_of_mem(p), NULL;
  if (!*mmp) {
    if (!(*mmp = mem_alloct(ma, map_t)) || !map_init(*mmp, ma, 8))
      return out_of_mem(p), NULL;
  }
  return *mmp;
}


static void fun_name(parser_t* p, fun_t* fun) {
  fun->name = p->scanner.sym;
  srcloc_t recv_loc = currloc(p);
  next(p);
  if (currtok(p) != TDOT) // plain function name, e.g. "foo"
    return;
  next(p);

  // method function name, e.g. "Foo.bar"

  sym_t recv_name = fun->name;
  fun->name = sym__; // in case of error

  // method name, e.g. "bar" in "Foo.bar"
  sym_t method_name = p->scanner.sym;
  srcloc_t method_name_loc = currloc(p);
  if (!expect(p, TID, "after '.'"))
    return;

  // resolve receiver, e.g. "Foo" in "Foo.bar"
  idexpr_t recvid = {0};
  recvid.kind = EXPR_ID;
  recvid.name = recv_name;
  recvid.loc = recv_loc;
  resolve_id(p, &recvid);
  if UNLIKELY(!recvid.ref)
    return;

  // check receiver
  type_t* recv = (type_t*)recvid.ref;
  if UNLIKELY(!nodekind_istype(recv->kind)) {
    const char* s = fmtnode(p, 0, (node_t*)recv, 1);
    error(p, (node_t*)&recvid, "%s is not a type", s);
  }
  fun->methodof = recv;

  // add method_name => fun to recv's method map
  map_t* mm = get_or_create_methodmap(p, recv);
  if (!mm)
    return;
  void** mp = map_assign_ptr(mm, p->scanner.compiler->ma, method_name);
  if UNLIKELY(!mp)
    return out_of_mem(p);
  if UNLIKELY(*mp) {
    const char* s = fmtnode(p, 0, (node_t*)recv, 1);
    recvid.loc = method_name_loc;
    return error(p, (node_t*)&recvid,
      "duplicate definition of method %s for type %s", method_name, s);
  }
  *mp = fun;

  // make canonical name
  buf_t* buf = &p->tmpbuf[0];
  buf_clear(buf);
  buf_print(buf, recv_name);
  buf_print(buf, "·"); // U+00B7 MIDDLE DOT (UTF8: "\xC2\xB7")
  if UNLIKELY(!buf_print(buf, method_name)) {
    out_of_mem(p);
  } else {
    fun->name = sym_intern(buf->p, buf->len);
  }
}


static bool fun_prototype(parser_t* p, fun_t* n) {
  if (currtok(p) == TID)
    fun_name(p, n);

  // parameters
  bool has_named_params = false;
  if UNLIKELY(!expect(p, TLPAREN, "for parameters")) {
    fastforward(p, (const tok_t[]){ TLBRACE, TSEMI, 0 });
    n->type = mkbad(p);
    return has_named_params;
  }
  if (currtok(p) != TRPAREN)
    has_named_params = fun_params(p, n);
  expect(p, TRPAREN, "to end parameters");

  // result type
  type_t* result = type_void;
  // check for "{}", e.g. "fun foo() {}" => "fun foo() void {}"
  if (currtok(p) != TLBRACE)
    result = type(p, PREC_MEMBER);

  n->type = (type_t*)funtype(p, &n->params, result);

  return has_named_params;
}


static type_t* type_fun(parser_t* p) {
  fun_t f = { .kind = EXPR_FUN, .loc = currloc(p) };
  next(p);
  fun_prototype(p, &f);
  return (type_t*)f.type;
}


// fundef = "fun" name "(" params? ")" result ( ";" | "{" body "}")
// result = params
// body   = (stmt ";")*
static expr_t* expr_fun(parser_t* p) {
  fun_t* n = mkexpr(p, fun_t, EXPR_FUN);
  next(p);
  bool has_named_params = fun_prototype(p, n);

  // define named function (must have type at this point)
  if (n->name && n->type->kind != NODE_BAD)
    define(p, n->name, (node_t*)n);

  if (has_named_params) {
    enter_scope(p);
    for (u32 i = 0; i < n->params.len; i++)
      define(p, ((local_t*)n->params.v[i])->name, n->params.v[i]);
  }

  if (currtok(p) != TSEMI) {
    if UNLIKELY(!has_named_params && n->params.len > 0)
      error(p, NULL, "function without named arguments can't have a body");

    fun_t* outer_fun = p->fun;
    p->fun = n;

    funtype_t* ft = (funtype_t*)n->type;

    bool hasthis = n->params.len && ((local_t*)n->params.v[0])->isthis;
    if (hasthis) {
      assertnotnull(n->methodof);
      dotctx_push(p, n->params.v[0]);
    }

    typectx_push(p, ft->result);
    n->body = expr(p, PREC_LOWEST);
    typectx_pop(p);

    if (hasthis)
      dotctx_pop(p);

    // check type of implicit return value
    if UNLIKELY(ft->kind == TYPE_FUN && ft->result != type_void &&
      n->body->type && !types_iscompat(ft->result, n->body->type) )
    {
      const char* restype = fmtnode(p, 0, (const node_t*)ft->result, 1);
      const char* bodytype = fmtnode(p, 1, (const node_t*)n->body->type, 1);
      node_t* origin = (node_t*)n->body;
      if (origin->kind == EXPR_BLOCK) {
        block_t* b = (block_t*)origin;
        if (b->children.len > 0) {
          origin = b->children.v[b->children.len-1];
          // // TODO: don't report error twice from "return"
          // if (origin->kind == EXPR_RETURN)
          //   origin = NULL;
        }
      }
      if (origin) {
        error(p, origin, "unexpected implicit function return type %s, expecting %s",
          bodytype, restype);
      }
    }

    p->fun = outer_fun;
  }

  if (has_named_params)
    leave_scope(p);

  return (expr_t*)n;
}


static stmt_t* stmt_fun(parser_t* p) {
  fun_t* n = (fun_t*)expr_fun(p);
  if UNLIKELY(n->kind == EXPR_FUN && !n->name)
    error(p, (node_t*)n, "anonymous function at top level");
  return (stmt_t*)n;
}


unit_t* parser_parse(parser_t* p, memalloc_t ast_ma, input_t* input) {
  p->ast_ma = ast_ma;
  scope_clear(&p->scope);
  scanner_set_input(&p->scanner, input);
  unit_t* unit = mknode(p, unit_t, NODE_UNIT);
  next(p);

  enter_scope(p);

  while (currtok(p) != TEOF) {
    stmt_t* n = stmt(p, PREC_LOWEST);
    push(p, &unit->children, n);
    if (!expect_token(p, TSEMI, "")) {
      fastforward_semi(p);
    } else {
      next(p);
    }
  }

  leave_scope(p);

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
    // types
    {"void", type_void},
    {"bool", type_bool},
    {"int",  type_int},
    {"uint", type_uint},
    {"i8",   type_i8},
    {"i16",  type_i16},
    {"i32",  type_i32},
    {"i64",  type_i64},
    {"u8",   type_u8},
    {"u16",  type_u16},
    {"u32",  type_u32},
    {"u64",  type_u64},
    {"f32",  type_f32},
    {"f64",  type_f64},
    // constants
    {"true",  const_true},
    {"false", const_false},
  };
  static void* storage[
    (MEMALLOC_BUMP_OVERHEAD + MAP_STORAGE_X(countof(entries))) / sizeof(void*)] = {0};
  memalloc_t ma = memalloc_bump(storage, sizeof(storage), MEMALLOC_STORAGE_ZEROED);
  safecheckx(map_init(&m, ma, countof(entries)));
  for (usize i = 0; i < countof(entries); i++) {
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
    goto err1;
  p->pkgdefs.parent = universe();
  if (!map_init(&p->tmpmap, c->ma, 32))
    goto err2;
  if (!map_init(&p->methodmap, c->ma, 32))
    goto err3;

  for (usize i = 0; i < countof(p->tmpbuf); i++)
    buf_init(&p->tmpbuf[i], c->ma);

  // note: p->typectxstack & dotctxstack are valid when zero initialized
  p->typectx = type_void;
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
  for (usize i = 0; i < countof(p->tmpbuf); i++)
    buf_dispose(&p->tmpbuf[i]);
  memalloc_t ma = p->scanner.compiler->ma;
  map_dispose(&p->pkgdefs, ma);
  map_dispose(&p->tmpmap, ma);
  map_dispose(&p->methodmap, ma);
  ptrarray_dispose(&p->typectxstack, ma);
  ptrarray_dispose(&p->dotctxstack, ma);
  scanner_dispose(&p->scanner);
}


// parselet tables


static const expr_parselet_t expr_parsetab[TOK_COUNT] = {
  // infix ops (in order of precedence from weakest to strongest)
  //[TCOMMA]     = {NULL, expr_infix_op, PREC_COMMA},
  [TASSIGN]    = {NULL, expr_infix_assign, PREC_ASSIGN}, // =
  [TMULASSIGN] = {NULL, expr_infix_assign, PREC_ASSIGN}, // *=
  [TDIVASSIGN] = {NULL, expr_infix_assign, PREC_ASSIGN}, // /=
  [TMODASSIGN] = {NULL, expr_infix_assign, PREC_ASSIGN}, // %=
  [TADDASSIGN] = {NULL, expr_infix_assign, PREC_ASSIGN}, // +=
  [TSUBASSIGN] = {NULL, expr_infix_assign, PREC_ASSIGN}, // -=
  [TSHLASSIGN] = {NULL, expr_infix_assign, PREC_ASSIGN}, // <<=
  [TSHRASSIGN] = {NULL, expr_infix_assign, PREC_ASSIGN}, // >>=
  [TANDASSIGN] = {NULL, expr_infix_assign, PREC_ASSIGN}, // &=
  [TXORASSIGN] = {NULL, expr_infix_assign, PREC_ASSIGN}, // ^=
  [TORASSIGN]  = {NULL, expr_infix_assign, PREC_ASSIGN}, // |=
  [TOROR]      = {NULL, expr_infix_op, PREC_LOGICAL_OR}, // ||
  [TANDAND]    = {NULL, expr_infix_op, PREC_LOGICAL_AND}, // &&
  [TOR]        = {NULL, expr_infix_op, PREC_BITWISE_OR}, // |
  [TXOR]       = {NULL, expr_infix_op, PREC_BITWISE_XOR}, // ^
  [TAND]       = {expr_ref, expr_infix_op, PREC_BITWISE_AND}, // &
  [TEQ]        = {NULL, expr_infix_op, PREC_EQUAL}, // ==
  [TNEQ]       = {NULL, expr_infix_op, PREC_EQUAL}, // !=
  [TLT]        = {NULL, expr_infix_op, PREC_COMPARE},   // <
  [TGT]        = {NULL, expr_infix_op, PREC_COMPARE},   // >
  [TLTEQ]      = {NULL, expr_infix_op, PREC_COMPARE}, // <=
  [TGTEQ]      = {NULL, expr_infix_op, PREC_COMPARE}, // >=
  [TSHL]       = {NULL, expr_infix_op, PREC_SHIFT}, // >>
  [TSHR]       = {NULL, expr_infix_op, PREC_SHIFT}, // <<
  [TPLUS]      = {expr_prefix_op, expr_infix_op, PREC_ADD}, // +
  [TMINUS]     = {expr_prefix_op, expr_infix_op, PREC_ADD}, // -
  [TSTAR]      = {expr_deref, expr_infix_op, PREC_MUL}, // *
  [TSLASH]     = {NULL, expr_infix_op, PREC_MUL}, // /
  [TPERCENT]   = {NULL, expr_infix_op, PREC_MUL}, // %

  // prefix and postfix ops (in addition to the ones above)
  [TPLUSPLUS]   = {expr_prefix_op, postfix_op, PREC_UNARY_PREFIX}, // ++
  [TMINUSMINUS] = {expr_prefix_op, postfix_op, PREC_UNARY_PREFIX}, // --
  [TNOT]        = {expr_prefix_op, NULL, PREC_UNARY_PREFIX}, // !
  [TTILDE]      = {expr_prefix_op, NULL, PREC_UNARY_PREFIX}, // ~
  [TMUT]        = {expr_mut, NULL, PREC_UNARY_PREFIX},
  [TLPAREN]     = {expr_group, expr_postfix_call, PREC_UNARY_POSTFIX}, // (

  // postfix ops
  [TLBRACK] = {NULL, expr_postfix_subscript, PREC_UNARY_POSTFIX}, // [

  // member ops
  [TDOT] = {expr_dotmember, expr_postfix_member, PREC_MEMBER}, // .

  // keywords & identifiers
  [TID]  = {expr_id, NULL, 0},
  [TFUN] = {expr_fun, NULL, 0},
  [TLET] = {expr_var, NULL, 0},
  [TVAR] = {expr_var, NULL, 0},
  [TIF]  = {expr_if, NULL, 0},
  [TFOR] = {expr_for, NULL, 0},
  [TRETURN] = {expr_return, NULL, 0},

  // constant literals
  [TINTLIT]   = {expr_intlit, NULL, 0},
  [TFLOATLIT] = {expr_floatlit, NULL, 0},

  // block
  [TLBRACE] = {expr_block, NULL, 0},
};


// type
static const type_parselet_t type_parsetab[TOK_COUNT] = {
  [TID]       = {type_id, NULL, 0},
  [TLBRACE]   = {type_struct, NULL, 0},
  [TFUN]      = {type_fun, NULL, 0},
  [TAND]      = {type_ref, NULL, 0},
  [TMUT]      = {type_mut, NULL, 0},
  [TQUESTION] = {type_optional, NULL, 0},
};


// statement
static const stmt_parselet_t stmt_parsetab[TOK_COUNT] = {
  [TFUN]  = {stmt_fun, NULL, 0},
  [TTYPE] = {stmt_typedef, NULL, 0},
};
