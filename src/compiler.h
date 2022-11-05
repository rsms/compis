#pragma once
#include "array.h"
#include "buf.h"

typedef u8 tok_t;
enum tok {
  #define _(NAME, ...) NAME,
  #define KEYWORD(str, NAME) NAME,
  #include "tokens.h"
  #undef _
  #undef KEYWORD
  TOK_COUNT,
};

#define FOREACH_NODE_KIND(_) \
  _( NBAD ) /* invalid node; parse error */ \
  \
  _( NCOMMENT ) \
  _( NINTLIT ) \
  _( NID ) \
  \
  _( NUNIT ) \
  _( NFUN ) \
// end FOREACH_NODE_KIND

typedef enum {
  #define _(NAME) NAME,
  FOREACH_NODE_KIND(_)
  #undef _
  NODEKIND_COUNT,
} nodekind_t;

ASSUME_NONNULL_BEGIN

typedef struct {
  mem_t data;
  bool  ismmap; // true if data is read-only mmap-ed
  char  name[];
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
  bool isblock; // true if this indent is a block
  u32  len;     // number of whitespace chars
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

typedef struct node node_t;
typedef struct {
  node_t* nullable head;
  node_t* nullable tail;
} nodelist_t;

struct node {
  nodekind_t       kind;     //
  srcloc_t         loc;      // source location; {0,0} if unknown
  node_t* nullable next;     // intrusive list link
  nodelist_t       children; //
  union {                    // value depends on kind
    u64   intval;
    char* strval; // allocated in ast_ma
  };
};

typedef struct {
  scanner_t  scanner;
  memalloc_t ast_ma; // AST allocator
} parser_t;


input_t* nullable input_create(memalloc_t ma, const char* filename);
void input_free(input_t* input, memalloc_t ma);
err_t input_open(input_t* input);
void input_close(input_t* input);

void compiler_init(compiler_t* c, memalloc_t, diaghandler_t);
void compiler_dispose(compiler_t* c);

void scanner_init(scanner_t* s, compiler_t* c);
void scanner_dispose(scanner_t* s);
void scanner_set_input(scanner_t* s, input_t*);
void scanner_next(scanner_t* s);

// scanner_lit returns the current token's corresponding source, which may be
// interpreted; i.e. source text `"\n"` => slice_t{.chars="\n", .len=1}
slice_t scanner_lit(const scanner_t* s);

void parser_init(parser_t* p, compiler_t* c);
void parser_dispose(parser_t* p);
node_t* parser_parse(parser_t* p, memalloc_t ast_ma, input_t*);

void node_free(memalloc_t ast_ma, node_t* n);

const char* tok_name(tok_t); // e.g. (TEQ) => "TEQ"
const char* tok_repr(tok_t); // e.g. (TEQ) => "="
usize tok_descr(char* buf, usize bufcap, tok_t, slice_t lit); // e.g. "number 3"
char* tok_descrs(char* buf, usize cap, tok_t, slice_t lit); // e.g. "number 3"

void report_errorv(compiler_t*, srcrange_t origin, const char* fmt, va_list);

ATTR_FORMAT(printf,3,4)
inline static void report_error(compiler_t* c, srcrange_t origin, const char* fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  report_errorv(c, origin, fmt, ap);
  va_end(ap);
}

ASSUME_NONNULL_END
