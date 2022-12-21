// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "buf.h"
#include "array.h"
#include "map.h"

// nodekind_t
#define FOREACH_NODEKIND(_) \
  _( NODE_BAD )/* invalid node; parse error */ \
  _( NODE_COMMENT )\
  _( NODE_UNIT )\
  _( STMT_TYPEDEF )\
  _( EXPR_FUN )/* nodekind_isexpr assumes this is first expr kind */\
  _( EXPR_BLOCK )\
  _( EXPR_CALL )\
  _( EXPR_ID )\
  _( EXPR_FIELD )\
  _( EXPR_PARAM )\
  _( EXPR_VAR )\
  _( EXPR_LET )\
  _( EXPR_MEMBER )\
  _( EXPR_PREFIXOP )\
  _( EXPR_POSTFIXOP )\
  _( EXPR_BINOP )\
  _( EXPR_ASSIGN )\
  _( EXPR_DEREF )\
  _( EXPR_IF )\
  _( EXPR_FOR )\
  _( EXPR_RETURN )\
  _( EXPR_BOOLLIT )\
  _( EXPR_INTLIT )\
  _( EXPR_FLOATLIT )\
// end FOREACH_NODEKIND
#define FOREACH_NODEKIND_TYPE(_) \
  /* primitive types */\
  _( TYPE_VOID ) /* must be first type kind */\
  _( TYPE_BOOL )\
  _( TYPE_I8  )\
  _( TYPE_I16 )\
  _( TYPE_I32 )\
  _( TYPE_I64 )\
  _( TYPE_INT )\
  _( TYPE_F32 )\
  _( TYPE_F64 )\
  /* user types */\
  _( TYPE_ARRAY ) /* nodekind_is*type assumes this is the first user type */\
  _( TYPE_FUN )\
  _( TYPE_PTR )\
  _( TYPE_REF )\
  _( TYPE_OPTIONAL )\
  _( TYPE_STRUCT )\
  _( TYPE_ALIAS )\
  /* special types replaced by typecheck */\
  _( TYPE_UNKNOWN ) /* nodekind_is*type assumes this is the first special type */\
  _( TYPE_UNRESOLVED ) /* named type not yet resolved */ \
// end FOREACH_NODEKIND_TYPE

typedef u8 tok_t;
enum tok {
  #define _(NAME, ...) NAME,
  #define KEYWORD(str, NAME) NAME,
  #include "tokens.h"
  #undef _
  #undef KEYWORD
  TOK_COUNT,
};

typedef u8 opflag_t;
#define OP_FL_WRITE ((opflag_t)1<< 0) // write semantics

typedef u8 op_t;
enum op {
  #define _(NAME, ...) NAME,
  #include "ops.h"
  #undef _
};

typedef u8 filetype_t;
enum filetype {
  FILE_OTHER,
  FILE_O,
  FILE_C,
  FILE_CO,
};

ASSUME_NONNULL_BEGIN

typedef struct {
  mem_t      data;
  bool       ismmap; // true if data is read-only mmap-ed
  filetype_t type;
  char       name[];
} input_t;

// loc_t is a compact representation of a source location: file, line, column & width.
// Inspired by the Go compiler's xpos & lico. (loc_t)0 is invalid.
typedef u64 loc_t;

// locmap_t maps loc_t to input_t
typedef array_type(input_t*) locmap_t; // slot 0 is always NULL

// origin_t describes the origin of diagnostic message (usually derived from loc_t)
typedef struct {
  const input_t* nullable input;
  u32 line;      // 0 if unknown (if so, other fields below are invalid)
  u32 column;
  u32 width;     // >0 if it's a range (starting at line & column)
  u32 focus_col; // if >0, signifies important column at loc_line(loc)
} origin_t;

// diaghandler_t is called when an error occurs. Return false to stop.
typedef struct diag diag_t;
typedef struct compiler compiler_t;
typedef void (*diaghandler_t)(const diag_t*, void* nullable userdata);
typedef enum { DIAG_ERR, DIAG_WARN, DIAG_HELP } diagkind_t;
typedef struct diag {
  compiler_t* compiler; // originating compiler instance
  const char* msg;      // descriptive message including "srcname:line:col: type:"
  const char* msgshort; // short descriptive message without source location
  const char* srclines; // source context (a few lines of the source; may be empty)
  origin_t    origin;   // origin of error (.line=0 if unknown)
  diagkind_t  kind;
} diag_t;

typedef struct {
  u32    cap;  // capacity of ptr (in number of entries)
  u32    len;  // current length of ptr (entries currently stored)
  u32    base; // current scope's base index into ptr
  void** ptr;  // entries
} scope_t;

typedef const char* sym_t;

// ———————— BEGIN AST ————————

typedef u8 nodekind_t;
enum nodekind {
  #define _(NAME) NAME,
  FOREACH_NODEKIND(_)
  FOREACH_NODEKIND_TYPE(_)
  #undef _
  NODEKIND_COUNT,
};

typedef u8 nodeflag_t;
#define NF_RVALUE      ((nodeflag_t)1<< 0) // expression is used as an rvalue
#define NF_OPTIONAL    ((nodeflag_t)1<< 1) // type-narrowed from optional
#define NF_EXITS       ((nodeflag_t)1<< 2) // block exits the function (has return)
#define NF_CHECKED     ((nodeflag_t)1<< 3) // has been typecheck'ed (or doesn't need it)
#define NF_UNKNOWN     ((nodeflag_t)1<< 4) // has or contains unresolved identifier
#define NF_NAMEDPARAMS ((nodeflag_t)1<< 5) // function has named parameters

// NODEFLAGS_BUBBLE are flags that "bubble" (transfer) from children to parents
#define NODEFLAGS_BUBBLE  NF_UNKNOWN

typedef struct {
  nodekind_t kind;
  nodeflag_t flags;
  u8         _unused[2];
  u32        nrefs; // used by expr_t and usertype_t
  loc_t      loc;
} node_t;

typedef struct {
  node_t;
} stmt_t;

typedef struct {
  node_t;
  ptrarray_t children;
} unit_t;

typedef struct {
  node_t;
  usize size;
  u8    align;
  bool  isunsigned; // only used by primitive types
  sym_t tid;
} type_t;

typedef struct {
  type_t;
  sym_t name;
} unresolvedtype_t;

typedef struct {
  type_t;
  sym_t   name;
  type_t* elem;
} aliastype_t;

typedef struct {
  type_t;
} usertype_t;

typedef struct {
  usertype_t;
  type_t* elem;
} arraytype_t;

typedef struct {
  usertype_t;
  ptrarray_t params;    // local_t*[]
  loc_t      paramsloc; // location of "(" ...
  type_t*    result;
} funtype_t;

typedef struct {
  usertype_t;
  sym_t nullable name;    // NULL if anonymous
  ptrarray_t     fields;  // field_t*[]
  bool           hasinit; // true if at least one field has an initializer
} structtype_t;

typedef struct {
  usertype_t;
  type_t* elem;
} ptrtype_t;

typedef struct {
  ptrtype_t;
  bool ismut;
} reftype_t;

typedef struct {
  usertype_t;
  type_t* elem;
} opttype_t;

typedef struct {
  stmt_t;
  aliastype_t type;
} typedef_t;

typedef struct {
  stmt_t;
  type_t* nullable type;
} expr_t;

typedef struct { expr_t; u64 intval; } intlit_t;
typedef struct { expr_t; union { double f64val; float f32val; }; } floatlit_t;
typedef struct { expr_t; sym_t name; node_t* nullable ref; } idexpr_t;
typedef struct { expr_t; op_t op; expr_t* expr; } unaryop_t;
typedef struct { expr_t; op_t op; expr_t* left; expr_t* right; } binop_t;
typedef struct { expr_t; expr_t* recv; ptrarray_t args; } call_t;
typedef struct { expr_t; expr_t* nullable value; } retexpr_t;

typedef struct { // block is a declaration (stmt) or an expression depending on use
  expr_t;
  ptrarray_t children;
  ptrarray_t cleanup; // cleanup_t[]
  scope_t    scope;
} block_t;

typedef struct {
  expr_t;
  expr_t*           cond;
  block_t*          thenb;
  block_t* nullable elseb;
} ifexpr_t;

typedef struct {
  expr_t;
  expr_t* nullable start;
  expr_t*          cond;
  expr_t*          body;
  expr_t* nullable end;
} forexpr_t;

typedef struct {
  expr_t;
  expr_t* recv;   // e.g. "x" in "x.y"
  sym_t   name;   // e.g. "y" in "x.y"
  expr_t* target; // e.g. "y" in "x.y"
} member_t;

typedef struct { // PARAM, VAR, LET
  expr_t;
  sym_t   nullable name;      // may be NULL for PARAM
  loc_t            nameloc;
  expr_t* nullable init;      // may be NULL for VAR and PARAM
  bool             isthis;    // [PARAM only] it's the special "this" parameter
  bool             ismut;     // [PARAM only] true if "this" parameter is "mut"
} local_t;

typedef struct { // fun is a declaration (stmt) or an expression depending on use
  expr_t;
  sym_t nullable    name;     // NULL if anonymous
  block_t* nullable body;     // NULL if function is a prototype
  type_t* nullable  methodof; // non-NULL for methods: type "this" is a method of
} fun_t;

// ———————— END AST ————————
// ———————— BEGIN IR ————————

typedef u8 irflag_t;
#define IR_FL_SEALED  ((irflag_t)1<< 0) // [block] is sealed

typedef u8 irblockkind_t;
enum irblockkind {
  IR_BLOCK_GOTO = 0, // plain continuation block with a single successor
  IR_BLOCK_RET,      // no successors, control value is memory result
  IR_BLOCK_SWITCH,   // N successors, switch(control) goto succs[N]
};

typedef struct irval_ irval_t;
typedef struct irval_ {
  u32      id;
  u32      nuse;
  irflag_t flags;
  op_t     op;
  u8       _reserved[2];
  u32      argc;
  irval_t* argv[3];
  loc_t    loc;
  type_t*  type;
  union {
    u32            i32val;
    u64            i64val;
    f32            f32val;
    f64            f64val;
    void* nullable ptr;
  } aux;

  struct {
    sym_t nullable live;
    sym_t nullable dst;
    sym_t nullable src;
  } var;

  const char* nullable comment;
} irval_t;

typedef struct irblock_ irblock_t;
typedef struct irblock_ {
  u32                 id;
  irflag_t            flags;
  irblockkind_t       kind;
  u8                  _reserved[2];
  loc_t               loc;
  irblock_t* nullable succs[2]; // successors (CFG)
  irblock_t* nullable preds[2]; // predecessors (CFG)
  ptrarray_t          values;
  irval_t* nullable   control;
    // control is a value that determines how the block is exited.
    // Its value depends on the kind of the block.
    // I.e. a IR_BLOCK_SWITCH has a boolean control value
    // while a IR_BLOCK_RET has a memory control value.
  const char* nullable comment;
} irblock_t;

typedef struct {
  fun_t*      ast;
  const char* name;
  ptrarray_t  blocks;
  u32         bidgen;     // block id generator
  u32         vidgen;     // value id generator
  u32         ncalls;     // # function calls that this function makes
  u32         npurecalls; // # function calls to functions marked as "pure"
  u32         nglobalw;   // # writes to globals
} irfun_t;

typedef struct {
  ptrarray_t functions;
} irunit_t;

// ———————— END IR ————————

typedef struct {
  input_t*  input;       // input source
  const u8* inp;         // input buffer current pointer
  const u8* inend;       // input buffer end
  const u8* linestart;   // start of current line
  tok_t     tok;         // recently parsed token (current token during scanning)
  loc_t     loc;         // recently parsed token's source location
  bool      insertsemi;  // insert a semicolon before next newline
  u32       lineno;      // monotonic line number counter (!= tok.loc.line)
} scanstate_t;

typedef struct {
  scanstate_t;
  compiler_t* compiler;
  const u8*   tokstart;    // start of current token
  const u8*   tokend;      // end of previous token
  usize       litlenoffs;  // subtracted from source span len in scanner_litlen()
  u64         litint;      // parsed INTLIT
  buf_t       litbuf;      // interpreted source literal (e.g. "foo\n")
  sym_t       sym;         // identifier
} scanner_t;

typedef struct {
  scanner_t        scanner;
  memalloc_t       ma; // general allocator (== scanner.compiler->ma)
  memalloc_t       ast_ma; // AST allocator
  scope_t          scope;
  map_t            pkgdefs;
  buf_t            tmpbuf[2];
  map_t            tmpmap;
  map_t            methodmap; // maps type_t* -> ptrarray_t of methods (fun_t*[])
  fun_t* nullable  fun;  // current function
  unit_t* nullable unit; // current unit
  type_t*          typectx;
  ptrarray_t       typectxstack;
  expr_t* nullable dotctx; // for ".name" shorthand
  ptrarray_t       dotctxstack;
  #if DEBUG
    int traceindent;
  #endif
} parser_t;

typedef struct {
  compiler_t*     compiler;
  parser_t*       p;
  memalloc_t      ma;     // p->scanner.compiler->ma
  memalloc_t      ast_ma; // p->ast_ma
  scope_t         scope;
  err_t           err;
  fun_t* nullable fun;  // current function
  type_t*         typectx;
  ptrarray_t      typectxstack;
  #if DEBUG
    int traceindent;
  #endif
} typecheck_t;

typedef struct {
  compiler_t* compiler;
  buf_t       outbuf;
  buf_t       headbuf;
  usize       headoffs;
  u32         headnest;
  u32         inputid;
  err_t       err;
  u32         anon_idgen;
  usize       indent;
  u32         lineno;
  u32         scopenest;
  map_t       typedefmap;
  map_t       tmpmap;
} cgen_t;

typedef struct compiler {
  memalloc_t     ma;          // memory allocator
  const char*    triple;      // target triple
  char*          cachedir;    // defaults to ".c0"
  char*          objdir;      // "${cachedir}/obj"
  char*          cflags;
  diaghandler_t  diaghandler; // called when errors are encountered
  void* nullable userdata;    // passed to diaghandler
  locmap_t       locmap;      // maps input <—> loc_t
  u32            errcount;    // number of errors encountered
  diag_t         diag;        // most recent diagnostic message
  buf_t          diagbuf;     // for diag.msg
  map_t          typeidmap;
  u32            intsize;     // byte size of "int" and "uint" types (register sized)
  u32            ptrsize;     // byte size of pointer, e.g. 8 for i64
  type_t*        addrtype;    // type for storing memory addresses, e.g. u64
  type_t*        inttype;     // type for "int"
  type_t*        uinttype;    // type for "uint"
  bool           isbigendian;
} compiler_t;


extern node_t* last_resort_node;

// universe constants
extern type_t* type_void;
extern type_t* type_unknown;
extern type_t* type_bool;
extern type_t* type_int;
extern type_t* type_uint;
extern type_t* type_i8;
extern type_t* type_i16;
extern type_t* type_i32;
extern type_t* type_i64;
extern type_t* type_u8;
extern type_t* type_u16;
extern type_t* type_u32;
extern type_t* type_u64;
extern type_t* type_f32;
extern type_t* type_f64;


// input
input_t* nullable input_create(memalloc_t ma, const char* filename);
void input_free(input_t* input, memalloc_t ma);
err_t input_open(input_t* input);
void input_close(input_t* input);
filetype_t filetype_guess(const char* filename);

// compiler
void compiler_init(compiler_t* c, memalloc_t, diaghandler_t);
void compiler_dispose(compiler_t* c);
void compiler_set_triple(compiler_t* c, const char* triple);
void compiler_set_cachedir(compiler_t* c, slice_t cachedir);
err_t compiler_compile(compiler_t*, promise_t*, input_t*, buf_t* ofile);

// scanner
bool scanner_init(scanner_t* s, compiler_t* c);
void scanner_dispose(scanner_t* s);
void scanner_set_input(scanner_t* s, input_t*);
void scanner_next(scanner_t* s);
slice_t scanner_lit(const scanner_t* s); // e.g. `"\n"` => slice_t{.chars="\n", .len=1}

// parser
bool parser_init(parser_t* p, compiler_t* c);
void parser_dispose(parser_t* p);
unit_t* parser_parse(parser_t* p, memalloc_t ast_ma, input_t*);

// post-parse passes
err_t typecheck(parser_t*, unit_t* unit);
err_t analyze(compiler_t*, unit_t* unit, memalloc_t ir_ma);

// ir
bool irfmt(buf_t* out, const irunit_t*, const locmap_t* nullable lm);
bool irfmt_dot(buf_t* out, const irunit_t*, const locmap_t* nullable lm); // graphviz
bool irfmt_fun(buf_t* out, const irfun_t*, const locmap_t* nullable lm);

// C code generator
bool cgen_init(cgen_t* g, compiler_t* c, memalloc_t out_ma);
void cgen_dispose(cgen_t* g);
err_t cgen_generate(cgen_t* g, const unit_t* unit);

// AST
const char* nodekind_name(nodekind_t); // e.g. "EXPR_INTLIT"
const char* nodekind_fmt(nodekind_t); // e.g. "variable"
err_t node_fmt(buf_t* buf, const node_t* nullable n, u32 depth); // e.g. i32, x, "foo"
err_t node_repr(buf_t* buf, const node_t* n); // S-expr AST tree
origin_t node_origin(const locmap_t*, const node_t*); // compute source origin of node
node_t* _mknode(parser_t* p, usize size, nodekind_t kind); // parser.c
node_t* clone_node(parser_t* p, const node_t* n);
// T* CLONE_NODE(T* node)
#define CLONE_NODE(p, nptr) ( \
  (__typeof__(nptr))memcpy( \
    _mknode((p), sizeof(__typeof__(*(nptr))), ((node_t*)(nptr))->kind), \
    (nptr), \
    sizeof(*(nptr))) )
local_t* nullable lookup_struct_field(structtype_t* st, sym_t name);
fun_t* nullable lookup_method(parser_t* p, type_t* recv, sym_t name);
expr_t* nullable lookup_member(parser_t* p, type_t* recv, sym_t name);

inline static bool nodekind_istype(nodekind_t kind) { return kind >= TYPE_VOID; }
inline static bool nodekind_isexpr(nodekind_t kind) {
  return EXPR_FUN <= kind && kind < TYPE_VOID; }
inline static bool nodekind_islocal(nodekind_t kind) {
  return kind == EXPR_FIELD || kind == EXPR_PARAM
      || kind == EXPR_LET   || kind == EXPR_VAR; }
inline static bool nodekind_isprimtype(nodekind_t kind) {
  return TYPE_VOID <= kind && kind < TYPE_ARRAY; }
inline static bool nodekind_isusertype(nodekind_t kind) {
  return TYPE_ARRAY <= kind && kind < TYPE_UNKNOWN; }
inline static bool nodekind_isspecialtype(nodekind_t kind) {
  return TYPE_UNKNOWN <= kind; }
inline static bool nodekind_isptrtype(nodekind_t kind) { return kind == TYPE_PTR; }
inline static bool nodekind_isreftype(nodekind_t kind) { return kind == TYPE_REF; }
inline static bool nodekind_isptrliketype(nodekind_t kind) {
  return kind == TYPE_PTR || kind == TYPE_REF; }
inline static bool nodekind_isvar(nodekind_t kind) {
  return kind == EXPR_VAR || kind == EXPR_LET; }

inline static bool node_istype(const node_t* n) {
  return nodekind_istype(assertnotnull(n)->kind); }
inline static bool node_isexpr(const node_t* n) {
  return nodekind_isexpr(assertnotnull(n)->kind); }
inline static bool node_isvar(const node_t* n) {
  return nodekind_isvar(assertnotnull(n)->kind); }
inline static bool node_islocal(const node_t* n) {
  return nodekind_islocal(assertnotnull(n)->kind); }
inline static bool node_isusertype(const node_t* n) {
  return nodekind_isusertype(n->kind); }

inline static bool type_isptr(const type_t* nullable t) {
  return nodekind_isptrtype(assertnotnull(t)->kind); }
inline static bool type_isref(const type_t* nullable t) {
  return nodekind_isreftype(assertnotnull(t)->kind); }
inline static bool type_isptrlike(const type_t* nullable t) {
  return nodekind_isptrliketype(assertnotnull(t)->kind); }
inline static bool type_isprim(const type_t* nullable t) {
  return nodekind_isprimtype(assertnotnull(t)->kind); }
inline static bool type_isopt(const type_t* nullable t) {
  return assertnotnull(t)->kind == TYPE_OPTIONAL; }
inline static bool type_isowner(const type_t* t) { // true for "*T" and "?*T"
  return type_isptr(type_isopt(t) ? ((opttype_t*)t)->elem : t);
}
inline static bool funtype_hasthis(const funtype_t* ft) {
  return ft->params.len && ((local_t*)ft->params.v[0])->isthis;
}

// types
bool types_isconvertible(const type_t* dst, const type_t* src);
bool _types_iscompat(const compiler_t* c, const type_t* dst, const type_t* src);
inline static bool types_iscompat(
  const compiler_t* c, const type_t* dst, const type_t* src)
{
  return dst == src || _types_iscompat(c, dst, src);
}

static sym_t nullable typeid(type_t* t);
sym_t nullable _typeid(type_t*);
inline static sym_t nullable typeid(type_t* t) { return t->tid ? t->tid : _typeid(t); }
#define TYPEID_PREFIX(typekind)  ('A'+(typekind)-TYPE_VOID)

// intern_usertype interns *tp in c->typeidmap.
// returns true if *tp was replaced by existing type, false if added to c->typeidmap.
bool intern_usertype(compiler_t* c, usertype_t** tp);

inline static const type_t* canonical_primtype(const compiler_t* c, const type_t* t) {
  return (t->kind == TYPE_INT) ? (t->isunsigned ? c->uinttype : c->inttype)
                               : t;
}

// expr_no_side_effects returns true if materializing n has no side effects
bool expr_no_side_effects(const expr_t* n);

// asexpr(void* ptr) -> expr_t*
// asexpr(const void* ptr) -> const expr_t*
#define asexpr(ptr) ( \
  assert(node_isexpr((const node_t*)assertnotnull(ptr))), \
  _Generic((ptr), \
    const node_t*: (const expr_t*)(ptr), \
    node_t*:       (expr_t*)(ptr), \
    const void*:   (const expr_t*)(ptr), \
    void*:         (expr_t*)(ptr) ) )


// tokens
const char* tok_name(tok_t); // e.g. TEQ => "TEQ"
const char* tok_repr(tok_t); // e.g. TEQ => "="
usize tok_descr(char* buf, usize bufcap, tok_t, slice_t lit); // e.g. "number 3"
inline static bool tok_isassign(tok_t t) { return TASSIGN <= t && t <= TORASSIGN; }

// operations
const char* op_name(op_t); // e.g. OP_ADD => "OP_ADD"
int op_name_maxlen();

// diagnostics
void report_diagv(compiler_t*, origin_t origin, diagkind_t, const char* fmt, va_list);
ATTR_FORMAT(printf,4,5) inline static void report_diag(
  compiler_t* c, origin_t origin, diagkind_t kind, const char* fmt, ...)
{
  va_list ap;
  va_start(ap, fmt);
  report_diagv(c, origin, kind, fmt, ap);
  va_end(ap);
}

// symbols
void sym_init(memalloc_t);
sym_t sym_intern(const void* key, usize keylen);
sym_t sym_snprintf(char* buf, usize bufcap, const char* fmt, ...)ATTR_FORMAT(printf,3,4);
extern sym_t sym__;    // "_"
extern sym_t sym_this; // "this"

// scope
void scope_clear(scope_t* s);
void scope_dispose(scope_t* s, memalloc_t ma);
bool scope_push(scope_t* s, memalloc_t ma);
void scope_pop(scope_t* s);
bool scope_stash(scope_t* s, memalloc_t ma);
void scope_unstash(scope_t* s);
bool scope_define(scope_t* s, memalloc_t ma, const void* key, void* value);
bool scope_undefine(scope_t* s, memalloc_t ma, const void* key);
void* nullable scope_lookup(scope_t* s, const void* key, u32 maxdepth);
inline static bool scope_istoplevel(const scope_t* s) { return s->base == 0; }

// pos
static void locmap_dispose(locmap_t* lm, memalloc_t ma);
inline static void locmap_clear(locmap_t* lm) { lm->len = 0; }
u32 locmap_inputid(locmap_t* lm, input_t*, memalloc_t); // get input id for source

static loc_t loc_make(u32 inputid, u32 line, u32 col, u32 width);

static input_t* nullable loc_input(loc_t p, const locmap_t*);
static u32 loc_line(loc_t p);
static u32 loc_col(loc_t p);
static u32 loc_width(loc_t p);
static u32 loc_inputid(loc_t p); // key for locmap_t; 0 for pos without input

static loc_t loc_with_inputid(loc_t p, u32 inputid); // copy of p with specific inputid
static loc_t loc_with_line(loc_t p, u32 line);       // copy of p with specific line
static loc_t loc_with_col(loc_t p, u32 col);         // copy of p with specific col
static loc_t loc_with_width(loc_t p, u32 width);     // copy of p with specific width

static void loc_set_line(loc_t* p, u32 line);
static void loc_set_col(loc_t* p, u32 col);
static void loc_set_width(loc_t* p, u32 width);

// loc_adjuststart returns a copy of p with its start and width adjusted by deltacol
loc_t loc_adjuststart(loc_t p, i32 deltacol); // cannot overflow (clamped)

// loc_union returns a loc_t that covers the column extent of both a and b
loc_t loc_union(loc_t a, loc_t b); // a and b must be on the same line

static loc_t loc_min(loc_t a, loc_t b);
static loc_t loc_max(loc_t a, loc_t b);
inline static bool loc_isknown(loc_t p) { return !!(loc_inputid(p) | loc_line(p)); }

// p is {before,after} q in same input
inline static bool loc_isbefore(loc_t p, loc_t q) { return p < q; }
inline static bool loc_isafter(loc_t p, loc_t q) { return p > q; }

// loc_fmt appends "file:line:col" to buf (behaves like snprintf)
usize loc_fmt(loc_t p, char* buf, usize bufcap, const locmap_t* lm);

// origin_make creates a origin_t
// 1. origin_make(const locmap_t*, loc_t)
// 2. origin_make(const locmap_t*, loc_t, u32 focus_col)
#define origin_make(...) __VARG_DISP(_origin_make,__VA_ARGS__)
#define _origin_make1 _origin_make2 // catch "too few arguments to function call"
origin_t _origin_make2(const locmap_t* m, loc_t loc);
origin_t _origin_make3(const locmap_t* m, loc_t loc, u32 focus_col);

origin_t origin_union(origin_t a, origin_t b);

//—————————————————————————————————————————————————————
// loc implementation

// Limits: inputs: 1048575, lines: 1048575, columns: 4095, width: 4095
// If this is too tight, we can either make lico 64b wide, or we can introduce a
// tiered encoding where we remove column information as line numbers grow bigger.
static const u64 _loc_widthBits   = 12;
static const u64 _loc_colBits     = 12;
static const u64 _loc_lineBits    = 20;
static const u64 _loc_inputidBits = 64 - _loc_lineBits - _loc_colBits - _loc_widthBits;

static const u64 _loc_inputidMax = (1llu << _loc_inputidBits) - 1;
static const u64 _loc_lineMax    = (1llu << _loc_lineBits) - 1;
static const u64 _loc_colMax     = (1llu << _loc_colBits) - 1;
static const u64 _loc_widthMax   = (1llu << _loc_widthBits) - 1;

static const u64 _loc_inputidShift = _loc_inputidBits + _loc_colBits + _loc_widthBits;
static const u64 _loc_lineShift    = _loc_colBits + _loc_widthBits;
static const u64 _loc_colShift     = _loc_widthBits;


inline static loc_t loc_make_unchecked(u32 inputid, u32 line, u32 col, u32 width) {
  return (loc_t)( ((loc_t)inputid << _loc_inputidShift)
              | ((loc_t)line << _loc_lineShift)
              | ((loc_t)col << _loc_colShift)
              | width );
}
inline static loc_t loc_make(u32 inputid, u32 line, u32 col, u32 width) {
  return loc_make_unchecked(
    MIN(_loc_inputidMax, inputid),
    MIN(_loc_lineMax, line),
    MIN(_loc_colMax, col),
    MIN(_loc_widthMax, width));
}
inline static u32 loc_inputid(loc_t p) { return p >> _loc_inputidShift; }
inline static u32 loc_line(loc_t p)    { return (p >> _loc_lineShift) & _loc_lineMax; }
inline static u32 loc_col(loc_t p)     { return (p >> _loc_colShift) & _loc_colMax; }
inline static u32 loc_width(loc_t p)   { return p & _loc_widthMax; }

// TODO: improve the efficiency of these
inline static loc_t loc_with_inputid(loc_t p, u32 inputid) {
  return loc_make_unchecked(
    MIN(_loc_inputidMax, inputid), loc_line(p), loc_col(p), loc_width(p));
}
inline static loc_t loc_with_line(loc_t p, u32 line) {
  return loc_make_unchecked(
    loc_inputid(p), MIN(_loc_lineMax, line), loc_col(p), loc_width(p));
}
inline static loc_t loc_with_col(loc_t p, u32 col) {
  return loc_make_unchecked(
    loc_inputid(p), loc_line(p), MIN(_loc_colMax, col), loc_width(p));
}
inline static loc_t loc_with_width(loc_t p, u32 width) {
  return loc_make_unchecked(
    loc_inputid(p), loc_line(p), loc_col(p), MIN(_loc_widthMax, width));
}

inline static void loc_set_line(loc_t* p, u32 line) { *p = loc_with_line(*p, line); }
inline static void loc_set_col(loc_t* p, u32 col) { *p = loc_with_col(*p, col); }
inline static void loc_set_width(loc_t* p, u32 width) { *p = loc_with_width(*p, width); }

inline static input_t* nullable loc_input(loc_t p, const locmap_t* lm) {
  u32 id = loc_inputid(p);
  return lm->len > id ? lm->v[id] : NULL;
}

inline static loc_t loc_min(loc_t a, loc_t b) {
  // pos-1 causes (loc_t)0 to become the maximum value of loc_t,
  // effectively preferring >(loc_t)0 over (loc_t)0 here.
  return (b-1 < a-1) ? b : a;
}
inline static loc_t loc_max(loc_t a, loc_t b) {
  return (b > a) ? b : a;
}

inline static void locmap_dispose(locmap_t* lm, memalloc_t ma) {
  array_dispose(input_t*, (array_t*)lm, ma);
}



ASSUME_NONNULL_END
