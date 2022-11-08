#pragma once
#include "array.h"
#include "buf.h"

// nodekind_t
#define FOREACH_NODEKIND(_) \
  _( NODE_BAD )/* invalid node; parse error */ \
  _( NODE_COMMENT )\
  _( NODE_UNIT )\
  _( EXPR_FUN )\
  _( EXPR_BLOCK )\
  _( EXPR_ID )\
  _( EXPR_PREFIXOP )\
  _( EXPR_POSTFIXOP )\
  _( EXPR_INFIXOP )\
  _( EXPR_INTLIT )\
// end FOREACH_NODEKIND

// typekind_t
#define FOREACH_TYPEKIND(_) \
  _( TYPE_VOID )\
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
// end FOREACH_TYPEKIND

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
  const input_t* input;
  u32 line, col;
} srcloc_t;

typedef struct {
  srcloc_t start, focus, end;
} srcrange_t;

// diaghandler_t is called when an error occurs. Return false to stop.
typedef struct diag diag_t;
typedef struct compiler compiler_t;
typedef void (*diaghandler_t)(const diag_t*, void* nullable userdata);
typedef struct diag {
  compiler_t* compiler; // originating compiler instance
  const char* msg;      // descriptive message including "srcname:line:col: type:"
  const char* msgshort; // short descriptive message without source location
  const char* srclines; // source context (a few lines of the source; may be empty)
  srcrange_t  origin;   // origin of error (loc.line=0 if unknown)
} diag_t;

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

typedef struct {
  u32 len;
} indent_t;

DEF_ARRAY_TYPE(indent_t, indentarray)

typedef struct {
  compiler_t*   compiler;
  input_t*      input;       // input source
  const u8*     inp;         // input buffer current pointer
  const u8*     inend;       // input buffer end
  const u8*     linestart;   // start of current line
  const u8*     tokstart;    // start of current token
  const u8*     tokend;      // end of previous token
  usize         litlenoffs;  // subtracted from source span len in scanner_litlen()
  token_t       tok;         // recently parsed token (current token during scanning)
  bool          insertsemi;  // insert a semicolon before next newline
  u32           lineno;      // monotonic line number counter (!= tok.loc.line)
  indent_t      indent;      // current level
  indent_t      indentdst;   // unwind to level
  indentarray_t indentstack; // previous indentation levels
  u64           litint;      // parsed TINTLIT
  buf_t         litbuf;      // interpreted source literal (e.g. "foo\n")
} scanner_t;

typedef u8 typekind_t;
enum typekind {
  #define _(NAME) NAME,
  FOREACH_TYPEKIND(_)
  #undef _
  TYPEKIND_COUNT,
};

typedef u8 nodekind_t;
enum nodekind {
  #define _(NAME) NAME,
  FOREACH_NODEKIND(_)
  #undef _
  NODEKIND_COUNT,
};

typedef struct {
  typekind_t kind;
  int        size;
  int        align;
  bool       isunsigned;
  srcloc_t   loc;
} type_t;

typedef struct node node_t;

// nodearray_t
DEF_ARRAY_TYPE(node_t*, nodearray)

struct node {
  nodekind_t       kind;
  srcloc_t         loc;
  type_t* nullable type;
  union {
    // NUNIT, NBLOCK
    nodearray_t children;
    // NINTLIT
    u64 intval;
    // NSTRLIT
    char* strval; // allocated in ast_ma
    // NPREFIXOP, NPOSTFIXOP
    struct { tok_t op; node_t* expr; } op1;
    // NPOSTFIXOP
    struct { tok_t op; node_t* left; node_t* right; } op2;
    // NFUN
    struct {
      type_t*          result_type;
      //paramarray_t     params; // TODO
      node_t* nullable name; // NULL if anonymous
      node_t* nullable body; // NULL if function is a prototype
    } fun;
  };
};

typedef struct {
  scanner_t  scanner;
  memalloc_t ast_ma; // AST allocator
} parser_t;

typedef struct {
  compiler_t* compiler;
  buf_t       outbuf;
  err_t       err;
  u32         anon_idgen;
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
void scanner_init(scanner_t* s, compiler_t* c);
void scanner_dispose(scanner_t* s);
void scanner_set_input(scanner_t* s, input_t*);
void scanner_next(scanner_t* s);
slice_t scanner_lit(const scanner_t* s); // e.g. `"\n"` => slice_t{.chars="\n", .len=1}

// parser
void parser_init(parser_t* p, compiler_t* c);
void parser_dispose(parser_t* p);
node_t* parser_parse(parser_t* p, memalloc_t ast_ma, input_t*);

// C code generator
void cgen_init(cgen_t* g, compiler_t* c, memalloc_t out_ma);
void cgen_dispose(cgen_t* g);
err_t cgen_generate(cgen_t* g, const node_t* unit);

// AST
void node_free(memalloc_t ast_ma, node_t* n);
void type_free(memalloc_t ast_ma, type_t* t);
const char* node_name(const node_t* n); // e.g. "NINTLIT"
const char* type_name(const type_t* t); // e.g. "TYPE_BOOL"
err_t node_repr(buf_t* buf, const node_t* n);
inline static bool node_has_strval(const node_t* n) {
  return n->kind == EXPR_ID || n->kind == NODE_COMMENT;
}

// tokens
const char* tok_name(tok_t); // e.g. (TEQ) => "TEQ"
const char* tok_repr(tok_t); // e.g. (TEQ) => "="
usize tok_descr(char* buf, usize bufcap, tok_t, slice_t lit); // e.g. "number 3"
char* tok_descrs(char* buf, usize cap, tok_t, slice_t lit); // e.g. "number 3"

// diagnostics
void report_errorv(compiler_t*, srcrange_t origin, const char* fmt, va_list);
ATTR_FORMAT(printf,3,4)
inline static void report_error(compiler_t* c, srcrange_t origin, const char* fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  report_errorv(c, origin, fmt, ap);
  va_end(ap);
}

ASSUME_NONNULL_END
