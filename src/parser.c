// Parser
// SPDX-License-Identifier: Apache-2.0
#include "colib.h"
#include "compiler.h"
#include "path.h"

#ifdef DEBUG
#include "abuf.h"
#include "debugutil.h"
#endif

#include <stdlib.h>
#include <strings.h> // strncasecmp


//——————————————————————— AST stats ———————————————————————
#ifdef CO_DEBUG_AST_STATS
typedef struct { const char* name; usize size; } ast_typeinfo_t;

static int sort_ast_typeinfo(
  const ast_typeinfo_t* x, const ast_typeinfo_t* y, void* nullable ctx)
{
  return x->size < y->size ? -1 : y->size < x->size ? 1 : 0;
}

__attribute__((constructor)) static void print_ast_stats() {
  ast_typeinfo_t ast_types[] = {
    { "node_t", sizeof(node_t) },
    { "type_t", sizeof(type_t) },
    { "expr_t", sizeof(expr_t) },
    { "unit_t", sizeof(unit_t) },
    { "aliastype_t", sizeof(aliastype_t) },
    { "arraylit_t", sizeof(arraylit_t) },
    { "arraytype_t", sizeof(arraytype_t) },
    { "binop_t", sizeof(binop_t) },
    { "block_t", sizeof(block_t) },
    { "call_t", sizeof(call_t) },
    { "forexpr_t", sizeof(forexpr_t) },
    { "fun_t", sizeof(fun_t) },
    { "funtype_t", sizeof(funtype_t) },
    { "idexpr_t", sizeof(idexpr_t) },
    { "ifexpr_t", sizeof(ifexpr_t) },
    { "local_t", sizeof(local_t) },
    { "member_t", sizeof(member_t) },
    { "retexpr_t", sizeof(retexpr_t) },
    { "structtype_t", sizeof(structtype_t) },
    { "subscript_t", sizeof(subscript_t) },
    { "typecons_t", sizeof(typecons_t) },
    { "typedef_t", sizeof(typedef_t) },
    { "unaryop_t", sizeof(unaryop_t) },
    { "unresolvedtype_t", sizeof(unresolvedtype_t) },
    { "intlit_t", sizeof(intlit_t) },
    { "floatlit_t", sizeof(floatlit_t) },
    { "strlit_t", sizeof(strlit_t) },
  };

  co_qsort(ast_types, countof(ast_types), sizeof(ast_types[0]),
    (co_qsort_cmp)sort_ast_typeinfo, NULL);

  printf("——————————————————————— AST stats ———————————————————————\n");


  // #ifdef CO_DEBUG_AST_STATS
  {
    usize size_avg = 0;
    usize size_max = 0;
    int namew = 0;
    for (usize i = 0; i < countof(ast_types); i++) {
      namew = MAX(namew, (int)strlen(ast_types[i].name));
      size_avg += ast_types[i].size;
      size_max = MAX(size_max, ast_types[i].size);
    }
    size_avg /= countof(ast_types);

    for (usize i = 0; i < countof(ast_types); i++)
      printf("%-*s %3zu\n", namew, ast_types[i].name, ast_types[i].size);
    printf("%-*s    \n",  namew, "");
    printf("%-*s %3zu\n", namew, "average", size_avg);
    printf("%-*s %3zu\n", namew, "max", size_max);

    printf("\n");
  }
  // #else
  //   printf("(build with -DCO_DEBUG_AST_STATS for more details)\n");
  // #endif


  usize histogram_labels[countof(ast_types)] = {0};
  usize histogram_counts[countof(ast_types)] = {0};
  usize histogram_len = 0;
  for (usize i = 0; i < countof(ast_types); i++) {
    bool found = false;
    for (usize j = 0; j < histogram_len; j++) {
      if (ast_types[i].size == histogram_labels[j]) {
        histogram_counts[j]++;
        found = true;
        break;
      }
    }
    if (!found) {
      histogram_labels[histogram_len] = ast_types[i].size;
      histogram_counts[histogram_len] = 1;
      histogram_len++;
    }
  }
  buf_t buf = buf_make(memalloc_ctx());
  debug_histogram_fmt(&buf, histogram_labels, histogram_counts, histogram_len);
  fwrite(buf.p, buf.len, 1, stdout);
  buf_dispose(&buf);
  printf("—————————————————————————————————————————————————————————\n");
} // print_ast_stats
#endif // CO_DEBUG_AST_STATS
//——————————————————————— end AST stats ———————————————————————


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

#define tracex(fmt, va...) \
  _trace(opt_trace_parse, 2, "P", fmt, ##va)


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
static struct { node_t; u8 opaque[128]; } _last_resort_node = { .kind=NODE_BAD };
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


inline static pkg_t* currpkg(parser_t* p) {
  return p->scanner.srcfile->pkg;
}


static void next(parser_t* p) {
  scanner_next(&p->scanner);
}


static bool slice_eq_cstri(slice_t s, const char* cstr) {
  usize len = strlen(cstr);
  return s.len == len && strncasecmp(s.chars, cstr, len) == 0;
}


static tok_t lookahead(parser_t* p, u32 distance) {
  assert(distance > 0);
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


struct no_origin { int x; };
#define CURR_ORIGIN ((struct no_origin){0})


// const origin_t to_origin(typecheck_t*, T origin)
// where T is one of: origin_t | loc_t | node_t* (default)
#define to_origin(a, origin) ({ \
  __typeof__(origin) __tmp1 = (origin); \
  __typeof__(origin)* __tmp = &__tmp1; \
  const origin_t __origin = _Generic(__tmp, \
          origin_t*:  *(origin_t*)__tmp, \
    const origin_t*:  *(origin_t*)__tmp, \
          loc_t*:     origin_make(locmap(a), *(loc_t*)__tmp), \
    const loc_t*:     origin_make(locmap(a), *(loc_t*)__tmp), \
          default:    ast_origin(locmap(a), *(node_t**)__tmp) \
  ); \
  __origin; \
})


// void diag(parser_t* p, T origin, diagkind_t diagkind, const char* fmt, ...)
// where T is one of: origin_t | loc_t | node_t* | expr_t*
#define diag(p, origin, diagkind, fmt, args...) \
  _diag((p), to_origin((p), (origin)), (diagkind), (fmt), ##args)

ATTR_FORMAT(printf,4,5)
static void _diag(parser_t* p, origin_t origin, diagkind_t kind, const char* fmt, ...) {
  // don't report extra errors after EOF
  if (currtok(p) == TEOF && p->scanner.errcount > 0)
    return;
  va_list ap;
  va_start(ap, fmt);
  p->scanner.errcount += (kind == DIAG_ERR);
  report_diagv(p->scanner.compiler, origin, kind, fmt, ap);
  va_end(ap);
  // panic("");
}

#define error(p, fmt, args...)   _diag(p, curr_origin(p), DIAG_ERR, (fmt), ##args)
#define warning(p, fmt, args...) _diag(p, curr_origin(p), DIAG_WARN, (fmt), ##args)
#define help(p, fmt, args...)    _diag(p, curr_origin(p), DIAG_HELP, (fmt), ##args)

#define error_at(p, origin, fmt, args...)   diag(p, origin, DIAG_ERR, (fmt), ##args)
#define warning_at(p, origin, fmt, args...) diag(p, origin, DIAG_WARN, (fmt), ##args)
#define help_at(p, origin, fmt, args...)    diag(p, origin, DIAG_HELP, (fmt), ##args)


static const char* diag_srcfile_name(parser_t* p, loc_t loc, buf_t* tmpbuf) {
  const srcfile_t* srcfile = loc_srcfile(loc, locmap(p));
  if (srcfile) {
    if (srcfile->pkg && srcfile->pkg->dir.len > 0) {
      buf_print(tmpbuf, relpath(srcfile->pkg->dir.p));
      buf_push(tmpbuf, PATH_SEP);
    }
    buf_append(tmpbuf, srcfile->name.p, srcfile->name.len);
    if (buf_nullterm(tmpbuf))
      return tmpbuf->chars;
  }
  return "<input>";
}


static void stop_parsing(parser_t* p) {
  stop_scanning(&p->scanner);
}


static void set_err(parser_t* p, err_t err) {
  if (p->scanner.err == 0)
    p->scanner.err = err;
}


static void out_of_mem(parser_t* p) {
  set_err(p, ErrNoMem);
  error(p, "out of memory");
  stop_parsing(p); // end scanner, making sure we don't keep going
}


static const char* fmttok(parser_t* p, u32 bufindex, tok_t tok, slice_t lit) {
  buf_t* buf = tmpbuf_get(bufindex);
  buf_reserve(buf, 64);
  tok_descr(buf->p, buf->cap, tok, lit);
  return buf->chars;
}


static void unexpected(parser_t* p, const char* errmsg) {
  if (currtok(p) == TEOF && p->scanner.errcount > 0)
    return;
  const char* tokstr = fmttok(p, 0, currtok(p), scanner_lit(&p->scanner));
  int msglen = (int)strlen(errmsg);
  const char* msgsep =
    (msglen && *errmsg != ',' && *errmsg != ';') ? ", " :
    msglen > 0 ?                                   " " :
                                                   "";
  error(p, "unexpected %s%s%*s", tokstr, msgsep, msglen, errmsg);
}


static void expect_fail(parser_t* p, tok_t expecttok, const char* errmsg) {
  const char* want = fmttok(p, 0, expecttok, (slice_t){0});
  const char* got = fmttok(p, 1, currtok(p), scanner_lit(&p->scanner));
  int msglen = (int)strlen(errmsg);
  if (msglen && *errmsg != ',' && *errmsg != ';')
    msglen++;
  error(p, "expected %s%*s, got %s", want, msglen, errmsg, got);
}

static bool expect_token(parser_t* p, tok_t expecttok, const char* errmsg) {
  if LIKELY(currtok(p) == expecttok)
    return true;
  expect_fail(p, expecttok, errmsg);
  return false;
}

static bool expect(parser_t* p, tok_t expecttok, const char* errmsg) {
  bool ok = expect_token(p, expecttok, errmsg);
  next(p);
  return ok;
}


static bool expect2_fail(parser_t* p, tok_t tok, const char* errmsg) {
  unexpected(p, errmsg);
  fastforward(p, (const tok_t[]){ tok, TSEMI, 0 });
  if (currtok(p) == tok)
    next(p);
  return false;
}

static bool expect2(parser_t* p, tok_t tok, const char* errmsg) {
  if LIKELY(currtok(p) == tok) {
    next(p);
    return true;
  }
  return expect2_fail(p, tok, errmsg);
}


static bool extend_loc_to_endloc(parser_t* p, loc_t* loc) {
  if (loc_line(p->scanner.endloc) == loc_line(*loc)) {
    loc_set_width(loc, loc_col(p->scanner.endloc) - loc_col(*loc));
    return true;
  }
  return false;
}


// maybe_start_of_type returns true if tok can possibly be the start of a type decl
static bool maybe_start_of_type(parser_t* p, tok_t tok) {
  switch (tok) {
    case TLPAREN:   // (   tuple type
    case TLBRACK:   // [   array type
    case TAND:      // &   ref type
    case TMUT:      // mut ref type
    case TQUESTION: // ?   optional type
    case TSTAR:     // *   pointer type
    case TID:       //     named type
      return true;
  }
  return false;
}


node_t* nullable ast_mknode(memalloc_t ast_ma, usize size, nodekind_t kind) {
  node_t* n = mem_alloc_zeroed(ast_ma, size).p;
  if (n)
    n->kind = kind;
  return n;
}


// T* CLONE_NODE(parser_t* p, T* node)
#define CLONE_NODE(p, nptr) ( \
  (__typeof__(nptr))memcpy( \
    _mknode((p), sizeof(__typeof__(*(nptr))), ((node_t*)(nptr))->kind), \
    (nptr), \
    sizeof(*(nptr))) )


#define mknode(p, TYPE, kind)      ( (TYPE*)_mknode((p), sizeof(TYPE), (kind)) )
#define mkexpr(p, TYPE, kind, fl)  ( (TYPE*)_mkexpr((p), sizeof(TYPE), (kind), (fl)) )


static node_t* _mknode(parser_t* p, usize size, nodekind_t kind) {
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
  t->size = p->scanner.compiler->target.ptrsize;
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


static type_t* mkplaceholdertype(
  parser_t* p, templateparam_t* templateparam, loc_t loc)
{
  placeholdertype_t* t = mknode(p, placeholdertype_t, TYPE_PLACEHOLDER);
  t->flags |= NF_UNKNOWN;
  t->flags |= NF_CHECKED;
  t->templateparam = templateparam;
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
// pnodearray


static bool nodearray_copy(nodearray_t* dst, memalloc_t ma, const nodearray_t* src) {
  if (src->len == 0) {
    dst->len = 0;
  } else {
    if UNLIKELY(dst->cap != 0)
      mem_freex(ma, MEM(dst->v, (usize)dst->cap * sizeof(void*)));
    usize nbyte = (usize)src->len * sizeof(void*);
    void* v = mem_alloc(ma, nbyte).p;
    if UNLIKELY(!v)
      return false;
    memcpy(v, src->v, nbyte);
    dst->v = v;
    dst->len = src->len;
    dst->cap = src->len;
  }
  return true;
}

static nodearray_t pnodearray_alloc(parser_t* p) {
  if (p->free_nodearrays.cap <= p->free_nodearrays.len) {
    if (p->free_nodearrays.cap == 0) {
      usize cap = 64/sizeof(nodearray_t);
      // dlog("alloc %zu initial nodearrays", cap);
      p->free_nodearrays.v = mem_alloctv(p->ma, nodearray_t, cap);
      if UNLIKELY(!p->free_nodearrays.v)
        goto last_resort;
      p->free_nodearrays.cap = (u32)cap;
    } else {
      usize oldcount = p->free_nodearrays.cap;
      usize newcount = oldcount + 1;
      // dlog("alloc an extra nodearray");
      void* v = mem_resizev(
        p->ma, p->free_nodearrays.v, oldcount, newcount, sizeof(nodearray_t));
      if (!v)
        goto last_resort;
      p->free_nodearrays.v = v;
      p->free_nodearrays.cap += 1;
    }
  } else {
    // dlog("recycle nodearray with cap %u",
    //   p->free_nodearrays.v[p->free_nodearrays.len].cap);
    p->free_nodearrays.v[p->free_nodearrays.len].len = 0;
  }
  return p->free_nodearrays.v[p->free_nodearrays.len++];
last_resort:
  return (nodearray_t){0};
}

static void pnodearray_dispose(parser_t* p, nodearray_t* na) {
  if (p->free_nodearrays.len == 0) {
    nodearray_dispose(na, p->ma);
  } else {
    p->free_nodearrays.v[--p->free_nodearrays.len] = *na;
  }
}

static bool pnodearray_push(parser_t* p, nodearray_t* na, void* n) {
  bool ok = nodearray_push(na, p->ma, n);
  if UNLIKELY(!ok)
    out_of_mem(p);
  return ok;
}

static bool pnodearray_assignto(parser_t* p, nodearray_t* na, nodearray_t* dst) {
  bool ok = nodearray_copy(dst, p->ast_ma, na);
  if UNLIKELY(!ok)
    out_of_mem(p);
  pnodearray_dispose(p, na);
  return ok;
}

static void pnodearray_assign1(parser_t* p, nodearray_t* dst, void* n1) {
  if UNLIKELY(dst->cap != 0)
    mem_freex(p->ast_ma, MEM(dst->v, (usize)dst->cap * sizeof(void*)));
  usize nbyte = sizeof(void*);
  node_t** v = mem_alloc(p->ast_ma, nbyte).p;
  if UNLIKELY(!v)
    return;
  *v = n1;
  dst->v = v;
  dst->len = 1;
  dst->cap = 1;
}


// —————————————————————————————————————————————————————————————————————————————————————
// scope


static void enter_scope(parser_t* p) {
  if (!scope_push(&p->scope, p->ma))
    out_of_mem(p);
}


static void leave_scope(parser_t* p) {
  scope_pop(&p->scope);
}


static node_t* nullable lookup(parser_t* p, sym_t name) {
  node_t* n = scope_lookup(&p->scope, name, U32_MAX);
  // if not found locally, look in package scope and its parent universe scope
  if (!n) {
    if ((n = pkg_def_get(currpkg(p), name))) {
      node_upgrade_visibility(n, NF_VIS_PKG);
      trace("lookup \"%s\" in package => %s", name, nodekind_name(n->kind));
    } else {
      trace("lookup \"%s\" in package (not found)", name);
    }
  } else {
    trace("lookup \"%s\" in scope => %s", name, nodekind_name(n->kind));
  }
  return n;
}


static void define(parser_t* p, sym_t name, node_t* n) {
  if (name == sym__)
    return;

  trace("define %s %s", name, nodekind_name(n->kind));
  assert(n->kind != NODE_BAD);

  node_t* existing = scope_lookup(&p->scope, name, 0);
  if (existing)
    goto err_duplicate;

  if (!scope_define(&p->scope, p->ma, name, n))
    out_of_mem(p);

  // top-level definitions also goes into package scope
  if (scope_istoplevel(&p->scope) &&
      n->kind != NODE_IMPORTID && n->kind != STMT_IMPORT)
  {
    // trace("define in pkg %s %s", name, nodekind_name(n->kind));
    existing = n;
    err_t err = pkg_def_add(currpkg(p), p->ma, name, /*inout*/&existing);
    if (err)
      return out_of_mem(p);
    if (existing != n)
      goto err_duplicate;
  }

  return;

err_duplicate:
  error_at(p, n, "redefinition of \"%s\"", name);
  if (loc_line(existing->loc))
    help_at(p, existing, "\"%s\" previously defined here", name);
}


// —————————————————————————————————————————————————————————————————————————————————————


static void dotctx_push(parser_t* p, expr_t* nullable n) {
  if UNLIKELY(!ptrarray_push(&p->dotctxstack, p->ma, p->dotctx))
    out_of_mem(p);
  p->dotctx = n;
}

static void dotctx_pop(parser_t* p) {
  assert(p->dotctxstack.len > 0);
  p->dotctx = ptrarray_pop(&p->dotctxstack);
}


static expr_t* expr_call(parser_t* p, expr_t* recv, nodeflag_t fl);


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
    const parselet_t* parselet = &expr_parsetab[currtok(p)];
    if (parselet->infix == NULL || parselet->prec < prec)
      break;
    log_pratt_infix(p, "expr", parselet->infix, parselet->prec, prec);
    n = parselet->infix(p, parselet, n, fl);
  }

  // shorthand call syntax, e.g. "f arg1, arg2"
  if (p->experiments.shorthand_call_syntax) switch (currtok(p)) {
    case TID:
    case TINTLIT:
    case TFLOATLIT:
    case TBYTELIT:
    case TSTRLIT:
    case TCHARLIT:
    {
      bool was_in_shorthand_call = p->in_shorthand_call;
      p->in_shorthand_call = true;
      n = expr_call(p, n, fl);
      p->in_shorthand_call = false;

      // disallow nested shorthand calls; it's confusing.
      // e.g. "f a, b c" is equivalent to "f(a, b(c))" not "f(a, b)(c)"
      if UNLIKELY(was_in_shorthand_call) {
        error_at(p, n, "nested shorthand call");
        help_at(p, n, "put parentheses around arguments: %s", fmtnode(0, n));
      }
    }
  }
  return n;
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


static int membertype_intern_cmp(const void* aptr, const void* bptr, void* ctx) {
  const importedtype_t* a = *(importedtype_t**)aptr;
  const importedtype_t* b = *(importedtype_t**)bptr;
  __uint128_t a_key =
    (((__uint128_t)(uintptr)a->import->name) << 64) | ((__uint128_t)(uintptr)a->name);
  __uint128_t b_key =
    (((__uint128_t)(uintptr)b->import->name) << 64) | ((__uint128_t)(uintptr)b->name);
  return a_key < b_key ? -1 : (int)(b_key < a_key)/* 1 or 0 */;
}


static type_t* parse_membertype(parser_t* p, import_t* im, loc_t loc) {
  // foo.T
  loc = currloc(p);
  next(p); // consume "."
  if UNLIKELY(currtok(p) != TID) {
    expect_fail(p, TID, "");
    return mkbad(p);
  }
  // intern "x.y" types as the same one may appear very often.
  // Key is (addressof_importname_symbol, addressof_name_symbol)
  importedtype_t ref = { .import = im, .name = p->scanner.sym };
  importedtype_t* ref1 = &ref;
  importedtype_t** ent = array_sortedset_assign(
    importedtype_t*, &p->membertypes, p->ma, &ref1, membertype_intern_cmp, NULL);
  if UNLIKELY(!ent)
    return out_of_mem(p), mkbad(p);
  importedtype_t* t = *ent;
  if (!t) {
    t = mknode(p, importedtype_t, TYPE_IMPORTED);
    t->flags |= NF_VIS_PUB;
    t->loc = loc;
    t->import = im;
    t->name = p->scanner.sym;
    t->nameloc = currloc(p);
    *ent = t;
  }
  next(p); // consume name
  return (type_t*)t;
}


// static importedtype_t* nullable interned_membertype_get(
//   parser_t* p, sym_t pkgname, sym_t membername)
// {
//   static array_t membertypes = {}; // FIXME
//   importedtype_t
// }


static type_t* named_type1(parser_t* p, sym_t name, loc_t loc, bool parse_member) {
  if UNLIKELY(name == sym__) {
    error_at(p, loc, "cannot use placeholder name (\"_\") as type");
    goto unresolved;
  }
  const node_t* n = lookup(p, name);
  if (!n) {
    if UNLIKELY(parse_member && currtok(p) == TDOT) {
      // e.g. "unknownid.Foo"
      next(p); // consume "."
      if (currtok(p) == TID) next(p); // consume name
      error_at(p, loc, "unknown package \"%s\"", name);
      if (loc_line(loc)) {
        loc_set_line(&loc, 1);
        loc_set_col(&loc, 1);
        loc_set_width(&loc, 0);
        help_at(p, loc, "add 'import \"%s\"' to top of %s",
          name, diag_srcfile_name(p, loc, tmpbuf_get(0)));
      }
    }
    goto unresolved;
  }

  if (node_istype(n))
    return (type_t*)n;

  if (n->kind == NODE_IMPORTID)
    goto unresolved;

  if (n->kind == NODE_TPLPARAM)
    return mkplaceholdertype(p, (templateparam_t*)n, loc);

  if (parse_member && n->kind == STMT_IMPORT && currtok(p) == TDOT)
    return parse_membertype(p, (import_t*)n, loc);

  error_at(p, loc, "%s \"%s\" is not a type", nodekind_fmt(n->kind), name);
unresolved:
  return mkunresolvedtype(p, name, loc);
}


static type_t* named_type(parser_t* p, sym_t name, loc_t loc) {
  return named_type1(p, name, loc, /*parse_member*/false);
}

static type_t* named_type_or_member(parser_t* p, sym_t name, loc_t loc) {
  return named_type1(p, name, loc, /*parse_member*/true);
}


static type_t* type_id(parser_t* p) {
  sym_t name = p->scanner.sym;
  loc_t loc = currloc(p);
  next(p); // consume TID
  return named_type_or_member(p, name, loc);
}


// checks for & consumes a closing ">" (correctly handles ">>")
static void expect_closing_gt(parser_t* p) {
  switch (currtok(p)) {
    case TGT:
      next(p);
      break;
    case TSHR:
      // Handle e.g. "A<B<C>>" which yields a TSHR ">>" token, not two TGT tokens.
      // Convert ">>" to ">", effectively "consuming" the first ">".
      p->scanner.tok = TGT;
      break;
    default:
      unexpected(p, "");
      fastforward(p, (const tok_t[]){ TGT, TSHR, 0 });
  }
}


// templatetype = type "<" (type ("," type)* ","?)? ">"
static type_t* type_template_expansion_infix(parser_t* p, prec_t prec, type_t* recv) {
  templatetype_t* tt = mknode(p, templatetype_t, TYPE_TEMPLATE);
  tt->recv = (usertype_t*)recv; assert(node_isusertype((node_t*)recv));
  next(p); // consume "<"

  if (currtok(p) != TGT && currtok(p) != TSHR) {
    nodearray_t args = pnodearray_alloc(p);
    for (;;) {
      type_t* arg = type(p, PREC_COMMA);
      if (!pnodearray_push(p, &args, arg))
        break; // OOM

      if (currtok(p) != TCOMMA)
        break;
      next(p); // consume ","
      // allow trailing comma
      if (currtok(p) == TGT || currtok(p) == TSHR)
        break;
    }
    pnodearray_assignto(p, &args, &tt->args);
  }

  // consume closing ">"
  tt->endloc = currloc(p);
  expect_closing_gt(p);

  return (type_t*)tt;
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


static local_t* nullable lookup_struct_field(structtype_t* st, sym_t name) {
  for (u32 i = 0; i < st->fields.len; i++) {
    local_t* f = (local_t*)st->fields.v[i];
    if (f->name == name)
      return f;
  }
  return NULL;
}


// field = id ("," id)* type ("=" expr ("," expr))
// Returns true if at least one field has an initializer (e.g. "= initexpr")
static bool struct_fieldset(parser_t* p, structtype_t* st, nodearray_t* fields) {
  u32 fields_start = fields->len;

  // parse names, e.g "x, y, z" or just "x"
  for (;;) {
    local_t* f = mknode(p, local_t, EXPR_FIELD);
    f->name = p->scanner.sym;

    if UNLIKELY(!expect(p, TID, "")) {
      fastforward_semi(p);
      return false;
    }

    // Look for duplicate field definition.
    // Note: It is not possible for fields to collide with type functions since a
    // type function can only be defined after a type is defined and after its fields.
    for (u32 i = 0; i < fields->len; i++) {
      local_t* f2 = (local_t*)fields->v[i];
      if UNLIKELY(f->name == f2->name) {
        error_at(p, f, "duplicate field \"%s\" for type %s", f->name, fmtnode(0, st));
        if (loc_line(f2->loc))
          warning_at(p, (node_t*)f2, "previously defined here");
        break;
      }
    }

    pnodearray_push(p, fields, f);
    if (currtok(p) != TCOMMA)
      break;
    next(p);
  }

  // parse type, e.g. "int" in "x int"
  type_t* t = type(p, PREC_MEMBER);

  // apply type to fields
  for (u32 i = fields_start; i < fields->len; i++) {
    local_t* f = (local_t*)fields->v[i];
    f->type = t;
    bubble_flags(f, t);
    bubble_flags(st, f);
  }

  // check if there's an initializer, e.g. "= initexpr".
  // If not, stop here
  if (currtok(p) != TASSIGN)
    return false; // no field initializer

  // parse initializer(s)
  // Note that we support multiple initializers for multiple fields, e.g.
  // "x, y int = 1, 2"
  next(p); // consume "="
  u32 i = fields_start;
  for (;;) {
    if (i == fields->len) {
      error(p, "excess field initializer");
      expr(p, PREC_COMMA, NF_RVALUE);
      break;
    }
    local_t* f = (local_t*)fields->v[i++];
    f->init = expr(p, PREC_COMMA, NF_RVALUE);
    bubble_flags(f, f->init);
    bubble_flags(st, f->init);
    if (currtok(p) != TCOMMA)
      break;
    next(p);
  }
  if (i < fields->len)
    error(p, "missing field initializer");
  return true;
}


static fun_t* fun(parser_t*, nodeflag_t, type_t* nullable recvt, bool requirename);


static void type_struct1_funs(parser_t* p, structtype_t* st) {
  bool reported_fun = false;
  u32 first_fn_index = p->toplevel_stmts.len;

  for (;;) {
    if (!p->experiments.fun_in_struct && !reported_fun) {
        reported_fun = true;
        error(p, "functions are not allowed in struct definitions");
      }
      fun_t* fn = fun(p, 0, /*recvt*/(type_t*)st, /*requirename*/false);
      if (!expect(p, TSEMI, ""))
        break;
      // Append the function to the unit, not to the structtype.
      // This simplifies both the general implementation and code generation.
      if (!pnodearray_push(p, &p->toplevel_stmts, fn))
        break;
      bubble_flags(p->unit, fn);

      if (currtok(p) != TFUN) {
        if UNLIKELY(currtok(p) == TID) {
          error(p, "fields cannot be defined after type functions");
          if (first_fn_index < p->toplevel_stmts.len) {
            fun_t* fn1 = (fun_t*)p->toplevel_stmts.v[first_fn_index];
            assert_nodekind(fn1, EXPR_FUN);
            if (loc_line(fn1->loc)) {
              help_at(p, fn1->loc, "define field '%s' above this function",
                p->scanner.sym);
            } else if (loc_line(fn->loc)) {
              help_at(p, fn->loc, "a function was defined here");
            }
          }
          fastforward(p, (const tok_t[]){ TRBRACE, 0 });
        }
        break;
      }
  }
}


static type_t* type_struct1(parser_t* p, structtype_t* st) {
  nodearray_t fields = pnodearray_alloc(p);
  while (currtok(p) != TRBRACE) {
    // <fun>
    if UNLIKELY(currtok(p) == TFUN) {
      type_struct1_funs(p, st);
      break;
    }
    // <field> <type> ...
    st->hasinit |= struct_fieldset(p, st, &fields);
    if (currtok(p) != TSEMI)
      break;
    next(p);
  }
  pnodearray_assignto(p, &fields, &st->fields);
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
  t->size = p->scanner.compiler->target.ptrsize;
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
    // slice is represented as {uint len; T* p}
    // mutslice is represented as {uint len, cap; T* p}
    st->align = p->scanner.compiler->target.ptrsize;
    const type_t* uinttype = p->scanner.compiler->uinttype;
    assert(uinttype->align <= st->align);
    st->size = p->scanner.compiler->target.ptrsize + uinttype->size;
    if (ismut)
      st->size += uinttype->size;

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


// templateparams = "<" tparam ("," tparam)* ","? ">"
// tparam         = name ("=" (expr | type))
static void parse_templateparams(parser_t* p, nodearray_t* templateparams) {
  assert(currtok(p) == TLT);
  next(p); // consume "<"

  bool optional_started = false;

  for (;;) {
    templateparam_t* tparam = mknode(p, templateparam_t, NODE_TPLPARAM);
    tparam->flags |= NF_UNKNOWN;
    tparam->name = p->scanner.sym;
    if (tparam->name == sym__)
      error(p, "cannot use placeholder name (\"_\") as template parameter");
    if (!expect2(p, TID, "")) {
      tparam->name = sym__;
      break;
    }

    if (!pnodearray_push(p, templateparams, tparam))
      return;

    // optional "=" init
    if (currtok(p) == TASSIGN) {
      optional_started = true;
      next(p); // consume "="
      // TODO how do we know what to expect here? A type or an expression?
      // e.g. both these template parameters are reasonable:
      //   type Foo<T=int,Size=100>
      //     things [T Size]
      //
      // One alternative is to require a specifier, e.g.
      //   type Foo<A,B=type int,C=100>
      // Or even "type" for parameters:
      //   type Foo<A type, B type = int, C expr = 100>
      //
      // For now, assume type
      tparam->init = (node_t*)type(p, PREC_COMMA);
    } else if (optional_started) {
      error_at(p, tparam, "required parameter following optional parameter");
      if (tparam->loc) {
        // help pointing to just after the parameter name
        origin_t origin = origin_make(locmap(p), tparam->loc);
        origin.width = 0;
        origin.column += strlen(tparam->name);
        _diag(p, origin, DIAG_HELP,
          "make %s optional by adding a default value e.g. %s=int",
          tparam->name, tparam->name);
      }
    }

    if (currtok(p) != TCOMMA)
      break;
    next(p); // consume ","
    // allow trailing comma
    if (currtok(p) == TGT || currtok(p) == TSHR)
      break;
    // parse another parameter
  }

  // consume closing ">"
  expect_closing_gt(p);
}


static void define_templateparams(parser_t* p, nodearray_t templateparams) {
  for (u32 i = 0; i < templateparams.len; i++) {
    templateparam_t* tparam = (templateparam_t*)templateparams.v[i];
    define(p, tparam->name, (node_t*)tparam);
  }
}


// typedef = "type" id type
static stmt_t* stmt_typedef(parser_t* p) {
  // "type"
  typedef_t* n = mknode(p, typedef_t, STMT_TYPEDEF);
  next(p);

  // name
  loc_t nameloc = currloc(p);
  sym_t name = p->scanner.sym;
  if (!expect(p, TID, ""))
    name = sym__;

  // generic? parse template parameters
  nodearray_t templateparams = {0};
  if (currtok(p) == TLT) {
    templateparams = pnodearray_alloc(p);
    parse_templateparams(p, &templateparams);
  }

  // next is either a type definition or an alias
  if (currtok(p) == TLBRACE) {
    // struct
    // e.g. "type Foo { x, y int }"
    // special path for struct to avoid (typedef (alias x (struct x))),
    // instead we simply get (typedef (struct x))
    structtype_t* t = mknode(p, structtype_t, TYPE_STRUCT);
    define(p, name, (node_t*)t);
    t->loc = nameloc;
    t->name = name;
    next(p);
    n->type = (type_t*)t;
    if (templateparams.len) {
      enter_scope(p);
      define_templateparams(p, templateparams);
    }
    type_struct1(p, t);
    bubble_flags(n, t);
  } else {
    // alias
    // e.g. "type Foo int"
    // e.g. "type Foo [int]"
    // e.g. "type Foo &[int]"
    aliastype_t* t = mknode(p, aliastype_t, TYPE_ALIAS);
    t->templateparams = templateparams;
    define(p, name, (node_t*)t);
    t->loc = nameloc;
    t->name = name;
    n->type = (type_t*)t;
    if (templateparams.len) {
      enter_scope(p);
      define_templateparams(p, templateparams);
    }
    t->elem = type(p, PREC_COMMA);
    if UNLIKELY(t->elem == (type_t*)t) {
      // e.g. "type A A"
      error_at(p, t, "recursive alias");
      // break cycle to prevent stack overflow in type_isowner
      t->elem = type_unknown;
    }
    bubble_flags(n, t->elem);
    if UNLIKELY(type_isopt(t->elem)) {
      error_at(p, t->elem,
        "cannot define optional aliased type;"
        " instead, mark as optional at use sites with ?%s", name);
    }
  }

  if (templateparams.len > 0) {
    leave_scope(p);
    pnodearray_assignto(p, &templateparams, &((usertype_t*)n->type)->templateparams);
    n->type->flags |= NF_TEMPLATE;
  }

  return (stmt_t*)n;
}


static bool resolve_id(parser_t* p, idexpr_t* n) {
  n->ref = lookup(p, n->name);
  if (!n->ref || n->ref->kind == NODE_IMPORTID || n->ref->kind == STMT_IMPORT) {
    trace("identifier \"%s\" not yet known", n->name);
    n->ref = NULL; // undo setting n->ref
    n->type = type_unknown;
  } else if (node_isexpr(n->ref)) {
    n->type = ((expr_t*)n->ref)->type;
  } else if (nodekind_istype(n->ref->kind)) {
    n->type = (type_t*)n->ref;
  } else {
    error_at(p, n, "cannot use %s \"%s\" as an expression",
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


static expr_t* parse_vardef(parser_t* p, nodeflag_t fl) {
  nodekind_t kind;
  nodeflag_t fl1 = fl;
  switch (currtok(p)) {
    case TLET:   kind = EXPR_LET; break;
    case TCONST: kind = EXPR_LET; fl1 |= NF_CONST; break; // TODO: add EXPR_CONST
    default:     kind = EXPR_VAR;
  }
  local_t* n = mkexpr(p, local_t, kind, fl1);
  next(p);
  if (currtok(p) != TID) {
    unexpected(p, "expecting identifier");
    return mkbad(p);
  }
  n->name = p->scanner.sym;
  n->nameloc = currloc(p);
  next(p);
  bool ok = true;
  if (currtok(p) == TASSIGN) {
    next(p);
    n->init = expr(p, PREC_ASSIGN, fl | NF_RVALUE);
    bubble_flags(n, n->init);
  } else {
    n->typeloc = currloc(p);
    n->type = type(p, PREC_LOWEST);
    if (!extend_loc_to_endloc(p, &n->typeloc) && n->type->kind != TYPE_STRUCT)
      n->typeloc = n->type->loc;
    bubble_flags(n, n->type);
    if (currtok(p) == TASSIGN) {
      next(p);
      n->init = expr(p, PREC_ASSIGN, fl | NF_RVALUE);
      bubble_flags(n, n->init);
    }
  }

  if UNLIKELY(fl & NF_RVALUE)
    error_at(p, n, "cannot use %s definition as value", nodekind_fmt(n->kind));

  define(p, n->name, (node_t*)n);

  // check for required initializer expression
  if (!n->init && ok) {
    // if UNLIKELY(n->kind == EXPR_LET) {
    //   error(p, "missing value for let binding, expecting '='");
    //   ok = false;
    // } else
    if UNLIKELY(type_isref(n->type)) {
      origin_t origin = n->typeloc ? to_origin(p, n->typeloc) : to_origin(p, n);
      origin.column += origin.width;
      origin.width = 0;
      error_at(p, origin, "missing initial value of reference type, expecting '='");
    }
  }

  return (expr_t*)n;
}

static expr_t* expr_var(parser_t* p, const parselet_t* pl, nodeflag_t fl) {
  return parse_vardef(p, fl);
}

static stmt_t* stmt_var(parser_t* p) {
  return (stmt_t*)parse_vardef(p, 0);
}


static block_t* block(parser_t* p, nodeflag_t fl) {
  block_t* n = mkexpr(p, block_t, EXPR_BLOCK, fl);
  next(p);

  if ((fl & NF_RVALUE) == 0)
    n->type = type_void;

  fl &= ~NF_RVALUE;
  bool reported_unreachable = false;
  bool exited = false;

  nodearray_t nary = pnodearray_alloc(p);

  if (currtok(p) != TRBRACE && currtok(p) != TEOF) {
    for (;;) {
      expr_t* cn = expr(p, PREC_LOWEST, fl);

      if (!pnodearray_push(p, &nary, cn))
        break;

      if (exited) {
        if (!reported_unreachable) {
          reported_unreachable = true;
          warning_at(p, (node_t*)cn, "unreachable code");
        }
      } else if (cn->kind == EXPR_RETURN) {
        exited = true;
      }

      expect(p, TSEMI, "");

      if (currtok(p) == TRBRACE || currtok(p) == TEOF)
        break;
    }
  }

  pnodearray_assignto(p, &nary, &n->children);
  n->endloc = currloc(p);
  expect2(p, TRBRACE, ", expected '}' or ';'");

  return n;
}


static block_t* any_as_block(parser_t* p, nodeflag_t fl) {
  if (currtok(p) == TLBRACE)
    return block(p, fl);
  block_t* n = mkexpr(p, block_t, EXPR_BLOCK, fl);
  expr_t* cn = expr(p, PREC_COMMA, fl);
  pnodearray_assign1(p, &n->children, cn);
  bubble_flags(n, cn);
  return n;
}


static expr_t* expr_block(parser_t* p, const parselet_t* pl, nodeflag_t fl) {
  enter_scope(p);
  block_t* n = block(p, fl);
  leave_scope(p);
  return (expr_t*)n;
}


static expr_t* expr_if(parser_t* p, const parselet_t* pl, nodeflag_t fl) {
  ifexpr_t* n = mkexpr(p, ifexpr_t, EXPR_IF, fl);
  next(p);

  // enter "then" scope
  enter_scope(p);

  // vardef or condition
  if (currtok(p) == TLET || currtok(p) == TVAR) {
    n->cond = expr_var(p, &expr_parsetab[TLET], fl & ~NF_RVALUE);
  } else {
    n->cond = expr(p, PREC_COMMA, fl | NF_RVALUE);
  }
  bubble_flags(n, n->cond);

  // "then" branch
  n->thenb = any_as_block(p, fl);
  bubble_flags(n, n->thenb);
  leave_scope(p);

  // "else" branch
  // allow else to follow on a new line, i.e. both of these are accepted:
  //
  //   if {} else {}
  //
  //   if {}
  //   else {}
  //
  if (currtok(p) != TELSE) {
    if (currtok(p) == TSEMI && lookahead(p, 1) == TELSE) {
      next(p);
    } else {
      return (expr_t*)n;
    }
  }
  next(p);
  enter_scope(p);
  n->elseb = any_as_block(p, fl);
  bubble_flags(n, n->elseb);
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
  n->nuse = 1;
  bubble_flags(n, n->value);
  return (expr_t*)n;
}


static expr_t* intlit(parser_t* p, nodeflag_t fl) {
  intlit_t* n = mkexpr(p, intlit_t, EXPR_INTLIT, fl);
  n->intval = p->scanner.litint;
  loc_set_width(&n->loc, scanner_lit(&p->scanner).len);
  next(p);
  return (expr_t*)n;
}


static expr_t* floatlit(parser_t* p, nodeflag_t fl) {
  floatlit_t* n = mkexpr(p, floatlit_t, EXPR_FLOATLIT, fl);
  char* endptr = NULL;

  // note: scanner always starts float litbuf with '+'
  if (fl & NF_NEG)
    p->scanner.litbuf.chars[0] = '-';

  n->f64val = strtod(p->scanner.litbuf.chars, &endptr);
  if UNLIKELY(endptr != p->scanner.litbuf.chars + p->scanner.litbuf.len) {
    error_at(p, n, "invalid floating-point constant");
  }
  // note: typecheck checks for overflow (HUGE_VAL)

  next(p);
  return (expr_t*)n;
}


static expr_t* expr_intlit(parser_t* p, const parselet_t* pl, nodeflag_t fl) {
  return intlit(p, fl & ~NF_NEG);
}


static expr_t* expr_floatlit(parser_t* p, const parselet_t* pl, nodeflag_t fl) {
  return floatlit(p, fl & ~NF_NEG);
}


static expr_t* expr_charlit(parser_t* p, const parselet_t* pl, nodeflag_t fl) {
  expr_t* n = intlit(p, fl & ~NF_NEG);
  n->flags |= NF_CHAR;
  return n;
}


static expr_t* expr_strlit(parser_t* p, const parselet_t* pl, nodeflag_t fl) {
  strlit_t* n = mkexpr(p, strlit_t, EXPR_STRLIT, fl);

  slice_t str = scanner_strval(&p->scanner);
  n->bytes = (u8*)mem_strdup(p->ast_ma, str, 0);
  n->len = str.len;

  // TODO: multiline string
  loc_set_width(&n->loc, n->len + 2);

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
  nodearray_t values = pnodearray_alloc(p);

  // parse values
  if (currtok(p) != TRBRACK) {
    fl |= NF_RVALUE;
    for (;;) {
      expr_t* val = expr(p, PREC_COMMA, fl);
      pnodearray_push(p, &values, val);
      bubble_flags(n, val);
      if (currtok(p) != TSEMI && currtok(p) != TCOMMA)
        break;
      next(p);
    }
  }

  pnodearray_assignto(p, &values, &n->values);

  // ']'
  n->endloc = currloc(p);
  expect(p, TRBRACK, "to end array literal");
  return (expr_t*)n;
}


static expr_t* expr_prefix_op(parser_t* p, const parselet_t* pl, nodeflag_t fl) {
  loc_t loc = currloc(p);
  next(p);

  // special case for negative number constants
  // e.g. "-123" becomes (INTLIT -123) instead of (PREFIXOP SUB (INTLIT 123))
  if (pl->op == OP_SUB) {
    switch (currtok(p)) {
      case TINTLIT: return intlit(p, fl | NF_RVALUE | NF_NEG);
      case TFLOATLIT: return floatlit(p, fl | NF_RVALUE | NF_NEG);
    }
  }

  unaryop_t* n = mkexpr(p, unaryop_t, EXPR_PREFIXOP, fl);
  n->op = pl->op;
  n->loc = loc;
  n->expr = expr(p, PREC_UNARY_PREFIX, fl | NF_RVALUE);
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
  n->op = ismut ? OP_MUTREF : OP_REF;
  next(p);
  n->expr = expr(p, PREC_UNARY_PREFIX, fl | NF_RVALUE);
  bubble_flags(n, n->expr);

  if UNLIKELY(n->expr->type->kind == TYPE_REF) {
    if (n->expr->type != type_unknown) {
      error_at(p, n, "referencing reference type %s", fmtnode(0, n->expr->type));
    } else {
      error_at(p, n, "referencing reference type");
    }
  } else if UNLIKELY(!expr_isstorage(n->expr)) {
    if (n->expr->type != type_unknown) {
      error_at(p, n, "referencing ephemeral value of type %s", fmtnode(0, n->expr->type));
    } else {
      error_at(p, n, "referencing ephemeral value");
    }
  } else if UNLIKELY(ismut && !expr_ismut(n->expr)) {
    const char* s = fmtnode(0, n->expr);
    nodekind_t k = n->expr->kind;
    if (k == EXPR_ID)
      k = ((idexpr_t*)n->expr)->ref->kind;
    error_at(p, n, "mutable reference to immutable %s %s", nodekind_fmt(k), s);
  }

  // // note: set type now, even though n->expr->type might be unknown,
  // // to communicate "&" vs "mut&" to typechecker
  // reftype_t* t = mkreftype(p, ismut);
  // t->elem = n->expr->type;
  // bubble_flags(t, t->elem);
  // n->type = (type_t*)t;

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


// named_param_or_id = id "=" expr | id
static expr_t* named_param_or_id(parser_t* p, nodeflag_t fl) {
  assert(currtok(p) == TID);

  usize nodesize = MAX(sizeof(idexpr_t), sizeof(local_t));
  idexpr_t* n = (idexpr_t*)_mkexpr(p, nodesize, EXPR_ID, fl);
  n->name = p->scanner.sym;
  next(p);

  if (currtok(p) != TASSIGN) {
    resolve_id(p, n);
    if UNLIKELY(currtok(p) == TCOLON) {
      error(p, "unexpected ':', did you mean '='?");
      next(p);
      return expr(p, PREC_COMMA, fl);
    }
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
// arg  = expr | id "=" expr
static void args(
  parser_t* p, call_t* n, type_t* recvtype, nodeflag_t fl, bool is_shorthand)
{
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

  nodearray_t args = pnodearray_alloc(p);

  // allow separating arguments with ';' for normal "f(...)" enclosed arguments,
  // but not for shorthand "f ..." arguments.
  tok_t extra_septok = is_shorthand ? TCOMMA : TSEMI;

  for (;;) {
    expr_t* arg;
    if (currtok(p) == TID) {
      // name:value
      arg = named_param_or_id(p, fl);
    } else {
      // value
      arg = expr(p, PREC_COMMA, fl);
    }

    pnodearray_push(p, &args, arg);
    bubble_flags(n, arg);

    if (currtok(p) != extra_septok && currtok(p) != TCOMMA)
      break;
    next(p);
  }

  pnodearray_assignto(p, &args, &n->args);
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
      error(p, "unexpected extra value in type cast");
    }
  } else {
    expect_fail(p, TRPAREN, "to end type cast");
  }
  fastforward_semi(p);
  return (expr_t*)n;
}


// call = expr "(" args? ")"
static expr_t* expr_call(parser_t* p, expr_t* recv, nodeflag_t fl) {
  type_t* recvtype = recv->type;

  // common case of primitive typecast
  idexpr_t* id = (idexpr_t*)recv;
  if (id->kind == EXPR_ID && id->ref && nodekind_isprimtype(id->ref->kind))
    return prim_typecons(p, (type_t*)id->ref, fl);

  call_t* n = mkexpr(p, call_t, EXPR_CALL, fl);
  bool is_shorthand = currtok(p) != TLPAREN;
  if (is_shorthand) {
    n->loc = recv->loc;
  } else {
    next(p); // consume "("
  }
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
    error_at(p, n, "calling %s; expected function or type",
      recv->type ? nodekind_fmt(recv->type->kind) : nodekind_fmt(recv->kind));
  }

  // args?
  if (currtok(p) != TRPAREN)
    args(p, n, recvtype, fl, is_shorthand);

  if (is_shorthand) {
    // note: we never get into the shorthand state without at least one argument
    assert(n->args.len > 0);
    n->argsendloc = n->args.v[n->args.len-1]->loc;
  } else {
    n->argsendloc = currloc(p);
    expect(p, TRPAREN, "to end function call");
  }

  return (expr_t*)n;
}


static expr_t* expr_postfix_call(
  parser_t* p, const parselet_t* pl, expr_t* recv, nodeflag_t fl)
{
  return expr_call(p, recv, fl);
}


// subscript = expr "[" expr "]"
static expr_t* expr_postfix_subscript(
  parser_t* p, const parselet_t* pl, expr_t* recv, nodeflag_t fl)
{
  subscript_t* n = mkexpr(p, subscript_t, EXPR_SUBSCRIPT, fl);
  next(p);

  recv->flags |= NF_RVALUE;

  n->recv = recv;
  n->index = expr(p, PREC_ASSIGN, fl | NF_RVALUE);
  bubble_flags(n, recv);

  // ']'
  n->endloc = currloc(p);
  expect(p, TRBRACK, "to end subscript");

  return (expr_t*)n;
}


// member = expr "." id
static expr_t* expr_postfix_member(
  parser_t* p, const parselet_t* pl, expr_t* left, nodeflag_t fl)
{
  member_t* n = mkexpr(p, member_t, EXPR_MEMBER, fl);
  next(p); // consume "."
  left->flags |= NF_RVALUE * (nodeflag_t)(left != p->dotctx);
  n->recv = left;
  n->name = p->scanner.sym;
  n->nameloc = currloc(p);
  bubble_flags(n, n->recv);
  expect(p, TID, "");
  return (expr_t*)n;
}


// dotmember = "." id
static expr_t* expr_dotmember(parser_t* p, const parselet_t* pl, nodeflag_t fl) {
  if UNLIKELY(!p->dotctx) {
    error(p, "\"this\" selector in regular function");
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

  // we accumulate fields in params, which are later copied to ft->params
  nodearray_t params = pnodearray_alloc(p);

  while (currtok(p) != TEOF) {
    local_t* param = mkexpr(p, local_t, EXPR_PARAM, 0);
    if UNLIKELY(param == NULL)
      goto oom;
    param->type = NULL; // clear type_void set by mkexpr for later check

    if (!pnodearray_push(p, &params, param))
      goto oom;

    // "mut this"?
    bool this_ismut = false;
    if (currtok(p) == TMUT && params.len == 1 && lookahead_issym(p, sym_this)) {
      this_ismut = true;
      next(p);
    }

    if (currtok(p) == TID) {
      // name, eg "x"; could be parameter name or type. Assume name for now.
      param->name = p->scanner.sym;
      param->loc = currloc(p);
      param->nameloc = param->loc;
      next(p);

      // check for "this" as first argument
      if (param->name == sym_this && params.len == 1 && recvt) {
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
          param->typeloc = currloc(p);
          param->type = type(p, PREC_LOWEST);
          extend_loc_to_endloc(p, &param->typeloc);
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
      param->typeloc = currloc(p);
      param->type = type(p, PREC_LOWEST);
      extend_loc_to_endloc(p, &param->typeloc);
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
  pnodearray_assignto(p, &params, &ft->params);
  if (isnametype) {
    // name-and-type form; e.g. "(x, y T, z Y)".
    // Error if at least one param has type, but last one doesn't, e.g. "(x, y int, z)"
    if UNLIKELY(typeq.len > 0) {
      error(p, "expecting type");
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
  pnodearray_dispose(p, &params);
  ptrarray_dispose(&typeq, p->ma);
  return false;
}


// funtype = "fun" "(" params? ")" result? ( ";" | "{" body "}")
//                 ↑
//               parsing starts here, after TFUN
// result = params
// body   = (stmt ";")*
static funtype_t* funtype(parser_t* p, loc_t loc, type_t* nullable recvt) {
  funtype_t* ft = mknode(p, funtype_t, TYPE_FUN);
  ft->loc = loc;
  ft->size = p->scanner.compiler->target.ptrsize;
  ft->align = p->scanner.compiler->target.ptrsize;
  ft->result = type_unknown;

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
  // no result type implies "void", e.g. "fun foo()" == "fun foo() void"
  if (maybe_start_of_type(p, currtok(p))) {
    ft->resultloc = currloc(p);
    ft->result = type(p, PREC_MEMBER);
    if (loc_line(ft->result->loc) && ft->result->loc > ft->loc)
      ft->resultloc = ft->result->loc;
    bubble_flags(ft, ft->result);
  }

  return ft;
}


static type_t* type_fun(parser_t* p) {
  loc_t loc = currloc(p);
  next(p); // consume TFUN
  funtype_t* ft = funtype(p, loc, /*recvt*/NULL);
  if UNLIKELY(ft->result == type_unknown)
    ft->result = type_void;
  return (type_t*)ft;
}


static void fun_body(parser_t* p, fun_t* n, nodeflag_t fl) {
  funtype_t* ft = (funtype_t*)n->type;

  if (funtype_hasthis(ft)) {
    assert(ft->params.len > 0);
    local_t* thisparam = (local_t*)ft->params.v[0];
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

  n->body = any_as_block(p, fl);

  // even though it may have implicit return, in practice a function body
  // block is never an expression itself.
  n->body->flags &= ~NF_RVALUE;
  bubble_flags(n, n->body);

  p->fun = outer_fun;

  if (funtype_hasthis(ft))
    dotctx_pop(p);
}


// fundef = "fun" [ type "." ] name "(" params? ")" result? ( ";" | "{" body "}")
// result = params
// body   = (stmt ";")*
static fun_t* fun(parser_t* p, nodeflag_t fl, type_t* nullable recvt, bool requirename) {
  fun_t* n = mkexpr(p, fun_t, EXPR_FUN, fl);
  next(p); // consume "fun"
  n->recvt = recvt;

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
          error_at(p, recv_nameloc, "no such type \"%s\"", recv_name);
        } else {
          error_at(p, recv_nameloc, "%s is not a type", recv_name);
        }
      }
    }
  } else if (currtok(p) == TLBRACK && n->recvt == NULL) {
    // type function for array type, e.g. "fun [T].name"
    n->recvt = type_array(p);
    n->name = sym__;
    // expect TDOT followed by TID
    if LIKELY(expect2(p, TDOT, ", expected '.name'")) {
      if (currtok(p) == TID) {
        n->name = p->scanner.sym;
        n->nameloc = currloc(p);
        next(p);
      } else {
        expect2_fail(p, TID, ", expected name");
      }
    }
  } else if (requirename) {
    error(p, "missing function name");
  }

  // parameters and result type
  funtype_t* ft = funtype(p, n->loc, n->recvt);
  n->type = (type_t*)ft;
  bubble_flags(n, n->type);

  // Copy params and source locations.
  // funtype may later become interned, so we keep local copies of these.
  // Note: params array itself is copied by typecheck so that we don't have to
  // typecheck the same function signature many times.
  n->paramsloc = ft->paramsloc;       // location of "(" ...
  n->paramsendloc = ft->paramsendloc; // location of ")"
  n->resultloc = ft->resultloc;       // location of result

  // named function (type function defined later, by typecheck)
  if (!n->recvt && n->name && n->type->kind != NODE_BAD)
    define(p, n->name, (node_t*)n);

  // no body?
  if (currtok(p) == TSEMI) {
    if (ft->result == type_unknown)
      ft->result = type_void;
    return n;
  }

  enter_scope(p);

  // define named parameters
  if (ft->flags & NF_NAMEDPARAMS) {
    for (u32 i = 0; i < ft->params.len; i++)
      define(p, ((local_t*)ft->params.v[i])->name, ft->params.v[i]);
  } else if UNLIKELY(ft->params.len > 0) {
    error(p, "function without named parameters can't have a body");
    origin_t origin = fun_params_origin(locmap(p), n);
    _diag(p, origin, DIAG_HELP, "name parameter%s \"_\"", ft->params.len > 1 ? "s" : "");
    // TODO: fix to_origin macro so that this works:
    // help_at(p, origin, "name parameter%s \"_\"", ft->params.len > 1 ? "s" : "");
  }

  if (currtok(p) == TASSIGN) {
    // e.g. "fun foo(x int) = x * x"
    if (!n->resultloc)
      n->resultloc = currloc(p);
    next(p); // consume "="
  } else {
    // e.g. "fun foo(x int) int { x * x }"
    // e.g. "fun foo(x int) { print(x) }"
    if UNLIKELY(currtok(p) != TLBRACE) {
      if (ft->result == type_unknown) {
        expect2_fail(p, TLBRACE, "expected type, '{' or '='");
      } else {
        expect2_fail(p, TLBRACE, "expected '{' or '='");
      }
      leave_scope(p);
      return n;
    }
    if (ft->result == type_unknown)
      ft->result = type_void;
  }

  fun_body(p, n, fl);

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

  // 'pub "ABI"'
  abi_t abi = ABI_CO;
  if (currtok(p) == TSTRLIT) {
    slice_t str = scanner_strval(&p->scanner);
    if (slice_eq_cstri(str, "C")) {
      abi = ABI_C;
    } else if (slice_eq_cstri(str, "CO")) {
      // abi is already "CO"
    } else {
      buf_t* buf = tmpbuf_get(0);
      if (!buf_appendrepr(buf, str.bytes, str.len)) {
        out_of_mem(p);
      } else {
        error_at(p, pub_loc, "invalid ABI: \"%.*s\"; expected \"C\" or \"co\"",
          (int)buf->len, buf->chars);
      }
    }
    next(p);
  }

  stmt_t* n = stmt(p);

  node_set_visibility((node_t*)n, NF_VIS_PUB);

  switch (n->kind) {
    case EXPR_FUN:
      ((fun_t*)n)->abi = abi;
      break;
    // case EXPR_VAR: // disallow public variables
    case EXPR_LET:
      if (!(n->flags & NF_CONST))
        goto err;
      ((local_t*)n)->abi = abi;
      break;
    case STMT_TYPEDEF:
      if (!(((typedef_t*)n)->type->flags & NF_VIS_PUB))
        node_set_visibility((node_t*)((typedef_t*)n)->type, NF_VIS_PUB);
      break;
    case NODE_BAD:
      break;
    default:
      goto err;
  }

  return n;
err:
  error_at(p, pub_loc, "unexpected pub qualifier on %s", nodekind_fmt(n->kind));
  return n;
}


// infer_import_name sets im->name based on im->path
//   "foo"         => "foo"
//   "foo/lol-cat" => "cat"
//   "foo!"        error: cannot infer imported name from path
static void infer_import_name(parser_t* p, import_t* im) {
  usize len = strlen(im->path);
  usize start = len;

  if UNLIKELY(len == 0)
    return error_at(p, im->pathloc, "empty import path");

  while (start > 0) {
    char c = im->path[start - 1];
    if (!isalnum(c) && c != '_')
      break;
    start--;
  }

  if UNLIKELY(start == len) {
    error_at(p, im->pathloc, "cannot infer package identifier from path");
    loc_t helploc = im->pathloc;
    if (loc_line(helploc)) {
      loc_set_col(&helploc, loc_col(helploc) + loc_width(helploc) + 1); // +1 for '"'
      loc_set_width(&helploc, 0);
      help_at(p, helploc, "add `as name` here");
    }
  }

  im->name = sym_intern(&im->path[start], len - start);
  im->nameloc = im->pathloc;
  if (len <= U32_MAX) {
    loc_set_col(&im->nameloc, loc_col(im->nameloc) + (u32)start);
    loc_set_width(&im->nameloc, (u32)(len - start));
  }
}


// parse_imports
//
// importstmt  = "import" (importgroup | importspec) ";"
// importgroup = "{" (importspec ";")+ "}"
// importspec  = posixpath ("as" id)? membergroup?
// membergroup = "{" (memberlist ";")+ "}"
// memberlist  = member ("," member)*
// member      = "*" | id ("as" id)?


// memberlist = member ("," member)*
// member     = "*" | id ("as" id)?
static importid_t* nullable parse_import_members(
  parser_t* p, import_t* im, importid_t* nullable id_tail)
{
  // "name1, name2 as alias, *"
  bool has_star = false;
  for (;;) {
    importid_t* id = mknode(p, importid_t, NODE_IMPORTID);
    id->flags |= NF_CHECKED | NF_UNKNOWN;
    id->orignameloc = id->loc;

    if (currtok(p) == TSTAR) {
      if (has_star)
        error(p, "duplicate \"*\" import");
      has_star = true;
      id->name = sym__;
      loc_set_width(&id->loc, 1); // "*"
      // special case for '*' which does not produce an automatic ';'.
      // e.g.
      //   import "foo" *
      //   import "bar"
      // scans as:
      //   IMPORT STRLIT("foo") STAR  // <— note: no ';'
      //   IMPORT STRLIT("bar")
      // so after parsing a trailing STAR, we convert it to a semicolon:
      // scans as:
      //   IMPORT STRLIT("foo") STAR SEMI
      //   IMPORT STRLIT("bar")
      tok_t nexttok = lookahead(p, 1);
      if (nexttok != TCOMMA && nexttok != TSEMI && nexttok != TID) {
        // transform TSTAR -> TSEMI
        p->scanner.tok = TSEMI;
      } else {
        next(p); // consume TSTAR
      }
    } else {
      id->name = p->scanner.sym;
      if (id->name == sym__)
        error(p, "invalid member \"_\" in import statement");
      next(p); // consume TID
    }

    // add to list
    if (id_tail) {
      id_tail->next_id = id;
    } else {
      im->idlist = id;
    }

    // alias ("origname as name")
    if (currtok(p) == TID && p->scanner.sym == sym_as) {
      if UNLIKELY(id->name == sym__) {
        // e.g. import * as x from "foo"
        error(p, "cannot alias \"*\" import");
      }
      next(p); // consume (ID as)
      expect_token(p, TID, "");
      if UNLIKELY(p->scanner.sym == sym__) {
        error(p, "cannot import a member as nothing (\"_\")");
        help_at(p, id->loc, "remove \"%s\" if you don't want it imported", id->name);
      }
      id->origname = id->name;
      id->name = p->scanner.sym;
      id->loc = currloc(p);
      next(p); // consume name
    }

    define(p, id->name, (node_t*)id);

    id_tail = id;

    // are we done?
    if (currtok(p) != TCOMMA)
      break;

    // expect to parse one more
    next(p); // consume ","
    if (currtok(p) != TID && currtok(p) != TSTAR) {
      unexpected(p, "expected identifier or '*'");
      fastforward_semi(p);
      return NULL;
    }
  }

  return id_tail;
}


// importspec  = posixpath ("as" id)? membergroup?
// membergroup = "{" (memberlist ";")* "}"
// memberlist  = member ("," member)*
// member      = "*" | id ("as" id)?
static import_t* parse_import_spec(
  parser_t* p, import_t* im, import_t* nullable list_tail)
{
  im->flags |= NF_CHECKED;
  im->name = sym__;

  // path e.g. "foo/cat" in "import foo/cat"
  if (!expect_token(p, TSTRLIT, ""))
    return im;
  slice_t strval = scanner_strval(&p->scanner);
  char* path = mem_strdup(p->ast_ma, strval, 0);
  if UNLIKELY(!path)
    return out_of_mem(p), im;
  im->path = path;
  im->pathloc = currloc(p);
  loc_set_col(&im->pathloc, loc_col(im->pathloc) + 1); // without '"'
  loc_set_width(&im->pathloc, (u32)strval.len);
  const char* errmsg;
  usize erroffs;
  if UNLIKELY(!import_validate_path(path, &errmsg, &erroffs)) {
    origin_t origin = origin_make(locmap(p), im->pathloc);
    if (erroffs <= (usize)U32_MAX)
      origin.focus_col = origin.column + (u32)erroffs;
    _diag(p, origin, DIAG_ERR, "invalid import path (%s)", errmsg);
    fastforward_semi(p);
    return im;
  }
  next(p);

  // just `import "path"`?
  if (currtok(p) == TSEMI) {
    infer_import_name(p, im);
    define(p, im->name, (node_t*)im);
    goto end;
  }

  // ("as" id)?
  if (currtok(p) == TID && p->scanner.sym == sym_as) {
    next(p); // consume "as"
    if (!expect_token(p, TID, ""))
      return im;
    im->name = p->scanner.sym;
    im->nameloc = currloc(p);
    loc_set_width(&im->nameloc, (u32)strlen(im->name));
    next(p); // consume identifier
    define(p, im->name, (node_t*)im);

    // next, we expect one of the following:
    // - ";" to end the import, e.g. `import "foo" as bar`
    // - "," to begin a list of specific members
    // - "{" to begin a group of list of specific members
    switch (currtok(p)) {
      case TCOMMA:  next(p); break;
      case TLBRACE: break;
      case TSEMI: goto end;
      default:
        unexpected(p, "expected end of import statement or '{ member ... }'");
        if (currtok(p) == TID || currtok(p) == TSTAR) {
          // provide helpful error for the following case:
          //   import "foo" as bar x  // user likely intended "x" to name a member
          loc_t helploc = im->nameloc;
          loc_set_col(&helploc, loc_col(helploc) + loc_width(helploc));
          loc_set_width(&helploc, 0);
          help_at(p, helploc, "wrap the members in braces, starting with a '{' here");
        }
        fastforward_semi(p);
        return im;
    }
  }

  // membergroup?
  if (currtok(p) == TLBRACE) {
    // membergroup = "{" (memberlist ";")* "}"
    // memberlist  = member ("," member)*
    // member      = "*" | id ("as" id)?
    next(p); // consume opening "{"
    importid_t* id_tail = NULL;
    if (currtok(p) != TRBRACE) for (;;) {
      id_tail = parse_import_members(p, im, id_tail);
      expect2(p, TSEMI, "");
      if (currtok(p) == TRBRACE) {
        next(p); // consume closing "}"
        break;
      }
    }
  }

end:
  // append to list
  if (list_tail == NULL) {
    p->unit->importlist = im;
  } else {
    list_tail->next_import = im;
  }
  return im;
}


static import_t* parse_import_stmt(parser_t* p, import_t* nullable list_tail) {
  import_t* im = mknode(p, import_t, STMT_IMPORT);
  next(p);

  // import "path" ...
  if (currtok(p) != TLBRACE)
    return parse_import_spec(p, im, list_tail);

  // import { ... }
  next(p); // consume opening "{"
  if (currtok(p) != TRBRACE) for (;;) {
    list_tail = parse_import_spec(p, im, list_tail);
    expect2(p, TSEMI, "");
    if (currtok(p) == TRBRACE) {
      next(p); // consume closing "}"
      break;
    }
    // we are going to parse another importspec; allocate memory for it
    import_t* im2 = mknode(p, import_t, STMT_IMPORT);
    im2->flags |= NF_CHECKED;
    im = im2;
  }
  return im;
}


static void parse_imports(parser_t* p) {
  import_t* tail = NULL;
  while (currtok(p) == TIMPORT) {
    tail = parse_import_stmt(p, tail);
    expect2(p, TSEMI, "");
  }
}


err_t parser_parse(parser_t* p, memalloc_t ast_ma, srcfile_t* srcfile, unit_t** result) {
  p->ast_ma = ast_ma;
  scope_clear(&p->scope);

  scanner_begin(&p->scanner, srcfile);

  unit_t* unit = mknode(p, unit_t, NODE_UNIT);
  unit->srcfile = srcfile;
  p->unit = unit;
  next(p);

  enter_scope(p);

  // first, parse any import statements
  parse_imports(p);

  p->toplevel_stmts = pnodearray_alloc(p);

  // next, parse rest of file
  while (currtok(p) != TEOF) {
    stmt_t* n = stmt(p);
    if (!pnodearray_push(p, &p->toplevel_stmts, n))
      break;
    bubble_flags(unit, n);
    expect2(p, TSEMI, "");
  }

  pnodearray_assignto(p, &p->toplevel_stmts, &unit->children);

  leave_scope(p);

  p->unit = NULL;
  *result = unit;

  return p->scanner.err;
}


bool parser_init(parser_t* p, compiler_t* c) {
  memset(p, 0, sizeof(*p));

  // TODO: make this set by a comment "//!enable_experiment fun_in_struct"
  p->experiments.fun_in_struct = true;
  p->experiments.shorthand_call_syntax = true;

  if (!scanner_init(&p->scanner, c))
    return false;

  if (!map_init(&p->tmpmap, c->ma, 32)) {
    scanner_dispose(&p->scanner);
    return false;
  }

  p->ma = p->scanner.compiler->ma;

  // note: dotctxstack is valid when zero-initialized
  p->dotctx = NULL;

  return true;
}


void parser_dispose(parser_t* p) {
  map_dispose(&p->tmpmap, p->ma);
  ptrarray_dispose(&p->dotctxstack, p->ma);
  ptrarray_dispose(&p->membertypes, p->ma);
  scanner_dispose(&p->scanner);
  scope_dispose(&p->scope, p->ma);
  array_dispose(nodearray_t, (array_t*)&p->free_nodearrays, p->ma);
}


// parselet tables


static const parselet_t expr_parsetab[TOK_COUNT] = {
  // infix ops (in order of precedence from weakest to strongest)
  // {prefix, infix, prec, op}
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
  [TCHARLIT]  = {expr_charlit},
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
  // {prefix, infix, prec}
  [TID]       = {type_id},       // T
  [TLBRACK]   = {type_array},    // [T N], [T]
  [TLBRACE]   = {type_struct},   // {...}
  [TFUN]      = {type_fun},      // fun(T)T
  [TSTAR]     = {type_ptr},      // *T
  [TAND]      = {type_ref},      // &T
  [TMUT]      = {type_mut},      // mut&T
  [TQUESTION] = {type_optional}, // ?T

  [TLT] = {NULL, type_template_expansion_infix, PREC_MEMBER}, // x<y...>
};


// statement
static const stmt_parselet_t stmt_parsetab[TOK_COUNT] = {
  // {prefix, infix, prec}
  [TPUB]   = {stmt_pub},
  [TFUN]   = {stmt_fun},
  [TTYPE]  = {stmt_typedef},
  [TCONST] = {stmt_var},
  [TLET]   = {stmt_var},
  [TVAR]   = {stmt_var},
};
