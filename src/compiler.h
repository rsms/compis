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
  _( TYPE_VOID ) /* must be first type kind */\
  _( TYPE_BOOL )\
  _( TYPE_I8  )\
  _( TYPE_I16 )\
  _( TYPE_I32 )\
  _( TYPE_I64 )\
  _( TYPE_F32 )\
  _( TYPE_F64 )\
  _( TYPE_INT )\
  _( TYPE_ARRAY ) /* nodekind_isusertype assumes this is first user type */\
  _( TYPE_ENUM )\
  _( TYPE_FUN )\
  _( TYPE_PTR )\
  _( TYPE_REF )\
  _( TYPE_OPTIONAL )\
  _( TYPE_STRUCT )\
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
// enum _op_count {
//   #define _(NAME, ...) _x##NAME,
//   #include "ops.h"
//   #undef _
//   OP_COUNT_
// };

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

typedef struct {
  const input_t* nullable input;
  u32 line, col;
} srcloc_t;

typedef struct {
  srcloc_t start, focus, end;
} srcrange_t;

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
  srcrange_t  origin;   // origin of error (loc.line=0 if unknown)
  diagkind_t  kind;
} diag_t;

typedef struct {
  u32    cap;  // capacity of ptr (in number of entries)
  u32    len;  // current length of ptr (entries currently stored)
  u32    base; // current scope's base index into ptr
  void** ptr;  // entries
} scope_t;

typedef struct compiler {
  memalloc_t     ma;          // memory allocator
  const char*    triple;      // target triple
  char*          cachedir;    // defaults to ".c0"
  char*          objdir;      // "${cachedir}/obj"
  char*          cflags;
  diaghandler_t  diaghandler; // called when errors are encountered
  void* nullable userdata;    // passed to diaghandler
  u32            errcount;    // number of errors encountered
  diag_t         diag;        // most recent diagnostic message
  buf_t          diagbuf;     // for diag.msg
  map_t          typeidmap;
  usize          ptrsize;     // byte size of pointer, e.g. 8 for i64
  bool           isbigendian;
} compiler_t;

typedef struct {
  tok_t    t;
  srcloc_t loc;
} token_t;

typedef const char* sym_t;

typedef struct {
  input_t*    input;       // input source
  const u8*   inp;         // input buffer current pointer
  const u8*   inend;       // input buffer end
  const u8*   linestart;   // start of current line
  token_t     tok;         // recently parsed token (current token during scanning)
  bool        insertsemi;  // insert a semicolon before next newline
  u32         lineno;      // monotonic line number counter (!= tok.loc.line)
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

// ———————— BEGIN AST ————————

typedef u8 nodekind_t;
enum nodekind {
  #define _(NAME) NAME,
  FOREACH_NODEKIND(_)
  FOREACH_NODEKIND_TYPE(_)
  #undef _
  NODEKIND_COUNT,
};

typedef u32 exprflag_t;
#define EX_RVALUE           ((exprflag_t)1<< 0) // expression is used as an rvalue
#define EX_OPTIONAL         ((exprflag_t)1<< 1) // type-narrowed from optional
#define EX_SHADOWS_OWNER    ((exprflag_t)1<< 2) // shadows the original owner of a value
#define EX_EXITS            ((exprflag_t)1<< 3) // block exits the function (has return)
#define EX_SHADOWS_OPTIONAL ((exprflag_t)1<< 4) // type-narrowed "if" check on optional
#define EX_DEAD_OWNER       ((exprflag_t)1<< 5) // node _was_ owner of TYPE_PTR
#define EX_OWNER_MOVED      ((exprflag_t)1<< 6) // owner moved
#define EX_ANALYZED         ((exprflag_t)1<< 7) // has been checked by the analyzer

typedef struct {
  nodekind_t kind;
  srcloc_t   loc;
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
  u32 nrefs;
} usertype_t;

typedef struct {
  usertype_t;
  type_t* elem;
} arraytype_t;

typedef struct {
  usertype_t;
  ptrarray_t params; // local_t*[]
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
  sym_t   name;
  type_t* type;
} typedef_t;

typedef struct {
  stmt_t;
  type_t* nullable type;
  u32              nrefs;
  exprflag_t       flags;
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
  expr_t* nullable init;      // may be NULL for VAR and PARAM
  bool             isthis;    // [PARAM only] it's the special "this" parameter
  //ownership_t      ownership; // [type==TYPE_PTR only] ownership status
} local_t;

typedef struct { // fun is a declaration (stmt) or an expression depending on use
  expr_t;
  ptrarray_t        params;   // local_t*[]
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
  srcloc_t loc;
  type_t*  type;
  irval_t* argv[3];
  u32      argc;
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

  // for temporary graph building
  ptrarray_t edges;
  ptrarray_t parents;

  const char* nullable comment;
} irval_t;

typedef struct irblock_ irblock_t;
typedef struct irblock_ {
  u32                 id;
  irflag_t            flags;
  irblockkind_t       kind;
  srcloc_t            loc;
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
} parser_t;

typedef struct {
  compiler_t* compiler;
  buf_t       outbuf;
  buf_t       headbuf;
  usize       headoffs;
  u32         headnest;
  err_t       err;
  u32         anon_idgen;
  usize       indent;
  u32         lineno;
  u32         scopenest;
  map_t       typedefmap;
  map_t       tmpmap;
  const input_t* nullable input;
} cgen_t;


extern node_t* last_resort_node;

// universe constants
extern type_t* type_void;
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

// analysis
err_t analyze(parser_t*, unit_t* unit);
err_t analyze2(compiler_t*, memalloc_t ir_ma, unit_t* unit);

// ir
bool irfmt(buf_t* out, const irunit_t*);
bool irfmt_fun(buf_t* out, const irfun_t*);
bool irfmt_dot(buf_t* out, const irfun_t*);

// C code generator
bool cgen_init(cgen_t* g, compiler_t* c, memalloc_t out_ma);
void cgen_dispose(cgen_t* g);
err_t cgen_generate(cgen_t* g, const unit_t* unit);

// AST
const char* nodekind_name(nodekind_t); // e.g. "EXPR_INTLIT"
const char* nodekind_fmt(nodekind_t); // e.g. "variable"
err_t node_fmt(buf_t* buf, const node_t* nullable n, u32 depth); // e.g. i32, x, "foo"
err_t node_repr(buf_t* buf, const node_t* n); // S-expr AST tree
srcrange_t node_srcrange(const node_t* n); // computes the source range for an AST
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
  return TYPE_ARRAY <= kind; }
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
inline static bool type_ismove(const type_t* t) {
  return nodekind_isusertype(t->kind);
}

// types
bool types_isconvertible(const type_t* dst, const type_t* src);
bool types_iscompat(const type_t* dst, const type_t* src);
sym_t nullable _typeid(type_t*);
inline static sym_t nullable typeid(type_t* t) { return t->tid ? t->tid : _typeid(t); }
bool typeid_append(buf_t* buf, type_t* t);
#define TYPEID_PREFIX(typekind)  ('A'+(typekind)-TYPE_VOID)

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

// ownership
inline static bool owner_islive(const void* expr) {
  return (asexpr(expr)->flags & EX_DEAD_OWNER) == 0;
}
inline static void owner_setlive(void* expr, bool live) {
  expr_t* n = asexpr(expr);
  n->flags = COND_FLAG(n->flags, EX_DEAD_OWNER, !live);
}

// tokens
const char* tok_name(tok_t); // e.g. TEQ => "TEQ"
const char* tok_repr(tok_t); // e.g. TEQ => "="
usize tok_descr(char* buf, usize bufcap, tok_t, slice_t lit); // e.g. "number 3"
inline static bool tok_isassign(tok_t t) { return TASSIGN <= t && t <= TORASSIGN; }

// operations
const char* op_name(op_t); // e.g. OP_ADD => "OP_ADD"
int op_name_maxlen();

// diagnostics
void report_diagv(compiler_t*, srcrange_t origin, diagkind_t, const char* fmt, va_list);
ATTR_FORMAT(printf,4,5) inline static void report_diag(
  compiler_t* c, srcrange_t origin, diagkind_t kind, const char* fmt, ...)
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


ASSUME_NONNULL_END
