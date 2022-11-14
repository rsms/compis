#pragma once
#include "buf.h"
#include "array.h"
#include "map.h"

// nodekind_t
#define FOREACH_NODEKIND(_) \
  _( NODE_BAD )/* invalid node; parse error */ \
  _( NODE_COMMENT )\
  _( NODE_UNIT )\
  _( EXPR_FUN )/* nodekind_isexpr assumes this is first expr kind */\
  _( EXPR_BLOCK )\
  _( EXPR_ID )\
  _( EXPR_PARAM )\
  _( EXPR_VAR )\
  _( EXPR_LET )\
  _( EXPR_PREFIXOP )\
  _( EXPR_POSTFIXOP )\
  _( EXPR_BINOP )\
  _( EXPR_INTLIT )\
  _( EXPR_FLOATLIT )\
// end FOREACH_NODEKIND
#define FOREACH_NODEKIND_TYPE(_) \
  _( TYPE_VOID ) /* must be first type kind */\
  _( TYPE_BOOL )\
  _( TYPE_INT )\
  _( TYPE_I8  )\
  _( TYPE_I16 )\
  _( TYPE_I32 )\
  _( TYPE_I64 )\
  _( TYPE_F32 )\
  _( TYPE_F64 )\
  _( TYPE_ARRAY )\
  _( TYPE_ENUM )\
  _( TYPE_FUNC )\
  _( TYPE_PTR )\
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
typedef enum { DIAG_ERR, DIAG_WARN } diagkind_t;
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
  diaghandler_t  diaghandler; // called when errors are encountered
  void* nullable userdata;    // passed to diaghandler
  u32            errcount;    // number of errors encountered
  diag_t         diag;        // most recent diagnostic message
  buf_t          diagbuf;     // for diag.msg
} compiler_t;

typedef struct {
  tok_t    t;
  srcloc_t loc;
} token_t;

typedef const char* sym_t;

typedef struct {
  compiler_t* compiler;
  input_t*    input;       // input source
  const u8*   inp;         // input buffer current pointer
  const u8*   inend;       // input buffer end
  const u8*   linestart;   // start of current line
  const u8*   tokstart;    // start of current token
  const u8*   tokend;      // end of previous token
  usize       litlenoffs;  // subtracted from source span len in scanner_litlen()
  token_t     tok;         // recently parsed token (current token during scanning)
  bool        insertsemi;  // insert a semicolon before next newline
  u32         lineno;      // monotonic line number counter (!= tok.loc.line)
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
  bool  isunsigned;
} type_t;

typedef struct {
  type_t;
  type_t* elem;
} arraytype_t;

typedef struct {
  stmt_t;
  type_t* nullable type;
  u32              nrefs;
} expr_t;

typedef struct { expr_t; u64 intval; } intlit_t;
typedef struct { expr_t; union { double f64val; float f32val; }; } floatlit_t;
typedef struct { expr_t; sym_t name; node_t* nullable ref; } idexpr_t;
typedef struct { expr_t; tok_t op; expr_t* expr; } unaryop_t;
typedef struct { expr_t; tok_t op; expr_t* left; expr_t* right; } binop_t;

typedef struct { // PARAM, VAR, LET
  expr_t;
  sym_t            name;
  expr_t* nullable init;
} local_t;

typedef struct { // block is a declaration (stmt) or an expression depending on use
  expr_t;
  ptrarray_t children;
} block_t;

typedef struct { // fun is a declaration (stmt) or an expression depending on use
  expr_t;
  type_t*          result_type; // TODO: remove and just use "type" field
  ptrarray_t       params;
  sym_t nullable   name; // NULL if anonymous
  expr_t* nullable body; // NULL if function is a prototype
} fun_t;

// ———————— END AST ————————

typedef struct {
  scanner_t  scanner;
  memalloc_t ast_ma; // AST allocator
  scope_t    scope;
  map_t      pkgdefs;
  buf_t      tmpbuf[2];
} parser_t;

typedef struct {
  compiler_t* compiler;
  buf_t       outbuf;
  err_t       err;
  u32         anon_idgen;
  usize       indent;
  u32         lineno;
  const input_t* nullable input;
} cgen_t;


extern node_t* last_resort_node;

extern type_t* const type_void;
extern type_t* const type_bool;
extern type_t* const type_int;
extern type_t* const type_uint;
extern type_t* const type_i8;
extern type_t* const type_i16;
extern type_t* const type_i32;
extern type_t* const type_i64;
extern type_t* const type_u8;
extern type_t* const type_u16;
extern type_t* const type_u32;
extern type_t* const type_u64;
extern type_t* const type_f32;
extern type_t* const type_f64;


// input
input_t* nullable input_create(memalloc_t ma, const char* filename);
void input_free(input_t* input, memalloc_t ma);
err_t input_open(input_t* input);
void input_close(input_t* input);
filetype_t filetype_guess(const char* filename);

// compiler
void compiler_init(compiler_t* c, memalloc_t, diaghandler_t);
void compiler_dispose(compiler_t* c);
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

// C code generator
void cgen_init(cgen_t* g, compiler_t* c, memalloc_t out_ma);
void cgen_dispose(cgen_t* g);
err_t cgen_generate(cgen_t* g, const unit_t* unit);

// AST
const char* nodekind_name(nodekind_t); // e.g. "EXPR_INTLIT"
const char* nodekind_fmt(nodekind_t); // e.g. "variable"
err_t node_fmt(buf_t* buf, const node_t* nullable n, u32 depth); // e.g. i32, x, "foo"
err_t node_repr(buf_t* buf, const node_t* n); // S-expr AST tree
inline static bool nodekind_istype(nodekind_t kind) { return kind >= TYPE_VOID; }
inline static bool nodekind_isexpr(nodekind_t kind) {
  return EXPR_FUN <= kind && kind < TYPE_VOID; }
inline static bool nodekind_islocal(nodekind_t kind) {
  return kind == EXPR_PARAM || kind == EXPR_VAR || kind == EXPR_LET; }
inline static bool node_istype(const node_t* n) { return nodekind_istype(n->kind); }
inline static bool node_isexpr(const node_t* n) { return nodekind_isexpr(n->kind); }
inline static bool node_islocal(const node_t* n) { return nodekind_islocal(n->kind); }

// tokens
const char* tok_name(tok_t); // e.g. (TEQ) => "TEQ"
const char* tok_repr(tok_t); // e.g. (TEQ) => "="
usize tok_descr(char* buf, usize bufcap, tok_t, slice_t lit); // e.g. "number 3"

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
extern sym_t sym__; // "_"

// scope
void scope_clear(scope_t* s);
void scope_dispose(scope_t* s, memalloc_t ma);
bool scope_push(scope_t* s, memalloc_t ma);
void scope_pop(scope_t* s);
bool scope_def(scope_t* s, memalloc_t ma, const void* key, void* value);
void* nullable scope_lookup(scope_t* s, const void* key);
inline static bool scope_istoplevel(const scope_t* s) { return s->base == 0; }

ASSUME_NONNULL_END
