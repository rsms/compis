// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "buf.h"
#include "str.h"
#include "array.h"
#include "map.h"
#include "strlist.h"
#include "subproc.h"
#include "target.h"
#include "thread.h"

// nodekind_t
#define FOREACH_NODEKIND(_) \
  _( NODE_BAD )/* invalid node; parse error */ \
  _( NODE_COMMENT )\
  _( NODE_UNIT )\
  _( STMT_TYPEDEF )\
  _( EXPR_FUN )/* nodekind_isexpr assumes this is first expr kind */\
  _( EXPR_BLOCK )\
  _( EXPR_CALL )\
  _( EXPR_TYPECONS )\
  _( EXPR_ID )\
  _( EXPR_FIELD )\
  _( EXPR_PARAM )\
  _( EXPR_VAR )\
  _( EXPR_LET )\
  _( EXPR_MEMBER )\
  _( EXPR_SUBSCRIPT )\
  _( EXPR_PREFIXOP )\
  _( EXPR_POSTFIXOP )\
  _( EXPR_BINOP )\
  _( EXPR_ASSIGN )\
  _( EXPR_DEREF ) /* implicit, by reading &T (user deref is EXPR_PREFIXOP) */\
  _( EXPR_IF )\
  _( EXPR_FOR )\
  _( EXPR_RETURN )\
  _( EXPR_BOOLLIT )\
  _( EXPR_INTLIT )\
  _( EXPR_FLOATLIT )\
  _( EXPR_STRLIT )\
  _( EXPR_ARRAYLIT )\
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
  _( TYPE_U8   )\
  _( TYPE_U16  )\
  _( TYPE_U32  )\
  _( TYPE_U64  )\
  _( TYPE_UINT )\
  _( TYPE_F32 )\
  _( TYPE_F64 )\
  /* user types */\
  _( TYPE_ARRAY )  /* nodekind_is*type assumes this is the first user type */\
  _( TYPE_FUN )\
  _( TYPE_PTR )\
  _( TYPE_REF )      /* &T      */\
  _( TYPE_MUTREF )   /* mut&T   */\
  _( TYPE_SLICE )    /* &[T]    */\
  _( TYPE_MUTSLICE ) /* mut&[T] */\
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

typedef struct pkg_t pkg_t;
typedef struct srcfile_t srcfile_t;
typedef struct fun_t fun_t;

// typefuntab_t maps types to sets of type functions.
// Each pkg_t has a typefuntab_t describing type functions defined by that package.
// Each unit_t has a typefuntab_t describing imported type functions.
typedef struct {
  map_t     m;  // { sym_t typeid => map_t*{ sym_t name => fun_t* } }
  rwmutex_t mu; // guards access to m
} typefuntab_t;

// loc_t is a compact representation of a source location: file, line, column & width.
// Inspired by the Go compiler's xpos & lico. (loc_t)0 is invalid.
typedef u64 loc_t;

// locmap_t maps loc_t to srcfile_t.
// All locmap_ functions are thread safe.
typedef struct {
  array_type(srcfile_t*) m;  // {loc_t => srcfile_t*} (slot 0 is always NULL)
  rwmutex_t              mu; // guards access to m
} locmap_t;

// origin_t describes the origin of diagnostic message (usually derived from loc_t)
typedef struct {
  srcfile_t* nullable file;
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

typedef struct srcfile_t {
  pkg_t*         pkg;    // parent package (set by pkg_add_srcfile)
  str_t          name;   // relative to pkg.dir (or absolute if there's no pkg.dir)
  void* nullable data;   // NULL until srcfile_open (and NULL after srcfile_close)
  usize          size;   // byte size of data (set by pkg_find_files, pkgs_for_argv)
  unixtime_t     mtime;  // modification time (set by pkg_find_files, pkgs_for_argv)
  u32            sha256[8];
  u32            id;     // index in pkg.files
  bool           ismmap; // true if srcfile_open used mmap
  filetype_t     type;   // file type (set by pkg_add_srcfile)
} srcfile_t;

typedef array_type(srcfile_t) srcfilearray_t;

typedef struct pkg_t {
  str_t           name;     // e.g. "main" or "std/runtime"
  str_t           dir;      // absolute path to source directory (empty for ad-hoc pkg)
  srcfilearray_t  files;    // source files
  map_t           defs;     // package-level definitions
  rwmutex_t       defs_mu;  // protects access to defs field
  typefuntab_t    tfundefs; // type functions defined by the package
  fun_t* nullable mainfun;  // fun main(), if any
} pkg_t;

typedef struct {
  u32    cap;  // capacity of ptr (in number of entries)
  u32    len;  // current length of ptr (entries currently stored)
  u32    base; // current scope's base index into ptr
  void** ptr;  // entries
} scope_t;

typedef const char* sym_t;

typedef u8 visibility_t;
enum visibility {
  VISIBILITY_PRIVATE = 0, // visible within same source file
  VISIBILITY_PKG     = 1, // visible within same package
  VISIBILITY_PUBLIC  = 2, // visible to other packages
};

typedef u8 abi_t;
enum abi {
  ABI_CO = 0,
  ABI_C  = 1,
};

typedef u8 buildmode_t;
enum buildmode {
  BUILDMODE_DEBUG,
  BUILDMODE_OPT,
};


#define EXPORT_ABI_C  ((export_t)1<< 3) // has been typecheck'ed (or doesn't need it)

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
#define NF_CHECKED     ((nodeflag_t)1<< 2) // has been typecheck'ed (or doesn't need it)
#define NF_UNKNOWN     ((nodeflag_t)1<< 3) // has or contains unresolved identifier
#define NF_NAMEDPARAMS ((nodeflag_t)1<< 4) // function has named parameters
#define NF_DROP        ((nodeflag_t)1<< 5) // type has drop() function
#define NF_SUBOWNERS   ((nodeflag_t)1<< 6) // type has owning elements
#define NF_EXIT        ((nodeflag_t)1<< 7) // block exits (i.e. "return" or "break")
#define NF_CONST       ((nodeflag_t)1<< 7) // [anything but block] is a constant

// NODEFLAGS_BUBBLE are flags that "bubble" (transfer) from children to parents
#define NODEFLAGS_BUBBLE  NF_UNKNOWN

typedef struct {
  nodekind_t kind;
  nodeflag_t flags;
  u8         _unused[2];
  u32        nuse; // number of uses (expr_t and usertype_t)
  loc_t      loc;
} node_t;

typedef struct {
  node_t;
} stmt_t;

typedef struct {
  node_t;
  ptrarray_t          children;
  scope_t             scope;
  srcfile_t* nullable srcfile;
  typefuntab_t        tfuns;   // imported type functions
} unit_t;

typedef struct {
  node_t;
  u64   size;
  u8    align;
  sym_t tid;
} type_t;

typedef struct {
  stmt_t;
  type_t* nullable type;
} expr_t;

typedef struct {
  type_t;
  sym_t            name;
  type_t* nullable resolved; // used by typeresolve
} unresolvedtype_t;

typedef struct {
  type_t;
  sym_t            name;
  type_t*          elem;
  node_t* nullable nsparent;
} aliastype_t;

typedef struct {
  type_t;
} usertype_t;

typedef struct {
  usertype_t;
  type_t* elem;
} ptrtype_t;

typedef struct {
  ptrtype_t;
  loc_t            endloc; // "]"
  u64              len;
  expr_t* nullable lenexpr;
} arraytype_t;

typedef struct {
  usertype_t;
  ptrarray_t params;       // local_t*[]
  loc_t      paramsloc;    // location of "(" ...
  loc_t      paramsendloc; // location of ")"
  type_t*    result;
  loc_t      resultloc;    // location of result
} funtype_t;

typedef struct {
  usertype_t;
  sym_t nullable   name;        // NULL if anonymous
  char* nullable   mangledname; // mangled name, created in ast_ma by typecheck
  ptrarray_t       fields;      // local_t*[]
  node_t* nullable nsparent;
  bool             hasinit;     // true if at least one field has an initializer
} structtype_t;

typedef struct {
  ptrtype_t;
} reftype_t;

typedef struct {
  ptrtype_t;
  loc_t endloc; // "]"
} slicetype_t;

typedef struct {
  ptrtype_t;
} opttype_t;

typedef struct {
  stmt_t;
  visibility_t visibility;
  union {
    type_t       type;
    aliastype_t  aliastype;
    structtype_t structtype;
  };
} typedef_t;

typedef struct {
  sym_t   name;
  type_t* type;
} drop_t;

typedef array_type(drop_t) droparray_t;
DEF_ARRAY_TYPE_API(drop_t, droparray)

typedef struct { expr_t; u64 intval; } intlit_t;
typedef struct { expr_t; double f64val; } floatlit_t;
typedef struct { expr_t; u8* bytes; usize len; } strlit_t;
typedef struct { expr_t; loc_t endloc; ptrarray_t values; } arraylit_t;
typedef struct { expr_t; sym_t name; node_t* nullable ref; } idexpr_t;
typedef struct { expr_t; op_t op; expr_t* expr; } unaryop_t;
typedef struct { expr_t; op_t op; expr_t* left; expr_t* right; } binop_t;
typedef struct { expr_t; expr_t* nullable value; } retexpr_t;

typedef struct {
  expr_t;
  expr_t*    recv;
  ptrarray_t args;
  loc_t      argsendloc; // location of ")"
} call_t;

typedef struct {
  expr_t;
  union {
    expr_t* nullable expr; // argument for primitive types
    ptrarray_t       args; // arguments for all other types
  };
} typecons_t;

typedef struct { // block is a declaration (stmt) or an expression depending on use
  expr_t;
  ptrarray_t  children;
  droparray_t drops;    // drop_t[]
  scope_t     scope;
  loc_t       endloc;   // location of terminating '}'
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
  expr_t*          recv;   // e.g. "x" in "x.y"
  sym_t            name;   // e.g. "y" in "x.y"
  expr_t* nullable target; // e.g. "y" in "x.y"
} member_t;

typedef struct {
  expr_t;
  expr_t* recv;      // e.g. "x" in "x[3]"
  expr_t* index;     // e.g. "3" in "x[3]"
  u64     index_val; // valid if index is a constant or comptime
  loc_t   endloc;    // location of terminating ']'
} subscript_t;

typedef struct { // PARAM, VAR, LET
  expr_t;
  sym_t   nullable name;    // may be NULL for PARAM
  loc_t            nameloc; // source location of name
  expr_t* nullable init;    // may be NULL for VAR and PARAM
  bool             isthis;  // [PARAM only] it's the special "this" parameter
  bool             ismut;   // [PARAM only] true if "this" parameter is "mut"
  u64              offset;  // [FIELD only] memory offset in bytes
} local_t;

typedef struct fun_t { // fun is a declaration (stmt) or an expression depending on use
  expr_t;
  sym_t nullable    name;         // NULL if anonymous
  loc_t             nameloc;      // source location of name
  block_t* nullable body;         // NULL if function is a prototype
  type_t* nullable  recvt;        // non-NULL for type functions (type of "this")
  char* nullable    mangledname;  // mangled name, created in ast_ma by typecheck
  abi_t             abi;
  visibility_t      visibility;
  node_t* nullable  nsparent;
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
    slice_t        bytes; // pointer into AST node, e.g. strlit_t
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
  ptrarray_t          functions;
  srcfile_t* nullable srcfile;
} irunit_t;

// ———————— END IR ————————

typedef struct {
  srcfile_t* srcfile;     // input source
  const u8*  inp;         // input buffer current pointer
  const u8*  inend;       // input buffer end
  const u8*  linestart;   // start of current line
  const u8*  tokstart;    // start of current token
  const u8*  tokend;      // end of previous token
  tok_t      tok;         // recently parsed token (current token during scanning)
  loc_t      loc;         // recently parsed token's source location
  bool       insertsemi;  // insert a semicolon before next newline
  u32        lineno;      // monotonic line number counter (!= tok.loc.line)
  u32        errcount;    // number of error diagnostics reported
  err_t      err;         // non-syntax error that occurred
} scanstate_t;

typedef struct {
  scanstate_t;
  compiler_t* compiler;
  u64         litint;      // parsed INTLIT
  buf_t       litbuf;      // interpreted source literal (e.g. "foo\n")
  sym_t       sym;         // current identifier value
} scanner_t;

typedef struct {
  scanner_t        scanner;
  memalloc_t       ma;     // general allocator (== scanner.compiler->ma)
  memalloc_t       ast_ma; // AST allocator
  scope_t          scope;
  map_t            tmpmap;
  fun_t* nullable  fun;      // current function
  unit_t* nullable unit;     // current unit
  expr_t* nullable dotctx;   // for ".name" shorthand
  ptrarray_t       dotctxstack;
  #if DEBUG
    int traceindent;
  #endif
} parser_t;

typedef struct {
  compiler_t*     compiler;
  pkg_t*          pkg;
  memalloc_t      ma;        // compiler->ma
  memalloc_t      ast_ma;    // compiler->ast_ma
  scope_t         scope;
  err_t           err;
  fun_t* nullable fun;       // current function
  type_t*         typectx;
  ptrarray_t      typectxstack;
  ptrarray_t      nspath;
  map_t           postanalyze; // set of nodes to analyze at the very end (keys only)
  map_t           tmpmap;
  bool            reported_error; // true if an error diagnostic has been reported
  #if DEBUG
    int traceindent;
  #endif
} typecheck_t;

typedef struct {
  compiler_t*  compiler;
  const pkg_t* pkg;
  memalloc_t   ma;         // compiler->ma
  buf_t        outbuf;
  buf_t        headbuf;
  usize        headoffs;
  u32          headnest;
  u32          headlineno;
  u32          headinputid;
  u32          inputid;
  u32          lineno;
  u32          scopenest;
  err_t        err;
  u32          anon_idgen;
  usize        indent;
  map_t        typedefmap;
  map_t        tmpmap;
  ptrarray_t   funqueue;   // [fun_t*] queue of (nested) functions awaiting build
} cgen_t;

typedef struct compiler {
  memalloc_t  ma;            // memory allocator
  buildmode_t buildmode;     // BUILDMODE_ constant
  char*       buildroot;     // where all generated files go, e.g. "build"
  char*       builddir;      // "{buildroot}/{mode}-{triple}"
  char*       sysroot;       // "{builddir}/sysroot"
  strlist_t   cflags;        // cflags used for compis objects (includes cflags_common)
  slice_t     flags_common;  // flags used for all objects; .s, .c etc.
  slice_t     cflags_common; // cflags used for all objects (includes xflags_common)
  slice_t     cflags_sysinc; // cflags with -isystemPATH for current target
  const char* ldname;        // name of linker for target ("" if none)
  int         lto;           // LTO level. 0 = disabled

  // diagnostics
  diaghandler_t  diaghandler; // called when errors are encountered
  void* nullable userdata;    // passed to diaghandler
  u32            errcount;    // number of errors encountered
  diag_t         diag;        // most recent diagnostic message
  buf_t          diagbuf;     // for diag.msg (also used as tmpbuf)

  // target info
  target_t    target;   // target triple
  type_t*     addrtype; // type for storing memory addresses, e.g. u64
  type_t*     inttype;  // "int"
  type_t*     uinttype; // "uint"
  slicetype_t u8stype;  // "&[u8]"
  aliastype_t strtype;  // "str"
  map_t       builtins;

  // configurable options (see compiler_config_t)
  bool opt_nolto : 1;
  bool opt_nomain : 1;
  bool opt_printast : 1;
  bool opt_printir : 1;
  bool opt_genirdot : 1;
  bool opt_genasm : 1;
  bool opt_nolibc : 1;
  bool opt_nolibcxx : 1;
  u8   opt_verbose; // 0=off 1=on 2=extra

  // data created during parsing & analysis
  // mutex must be locked when multiple threads share a compiler instance
  map_t     typeidmap; // sym_t typeid => type_t*
  rwmutex_t typeidmap_mu;
  locmap_t  locmap;    // maps loc_t => srcfile_t*
} compiler_t;

typedef struct { // compiler_config_t
  // Required fields
  const target_t* target;    // target to compile for
  const char*     buildroot; // directory for user build products

  // Optional fields; zero value is assumed to be a common default
  buildmode_t buildmode; // BUILDMODE_ constant. 0 = BUILDMODE_DEBUG

  // Options which maps to compiler_t.opt_
  bool nolto;    // prevent use of LTO, even if that would be the default
  bool nomain;   // don't auto-generate C ABI "main" for main.main
  bool printast;
  bool printir;
  bool genirdot;
  bool genasm;   // write machine assembly .S source file to build dir
  bool verbose;
  bool nolibc;
  bool nolibcxx;

  // sysver sets the minimum system version. Ignored if NULL or "".
  // Currently only supported for macos via -mmacosx-version-min=sysver.
  // Defaults to the target's sysver if not set (common case.)
  const char* nullable sysver;

  // sysroot sets a custom sysroot. Ignored if NULL or "".
  const char* nullable sysroot;
} compiler_config_t;


extern node_t* last_resort_node;

// name prefix reserved for implementation
// note: if you change this, also update coprelude.h
#define CO_INTERNAL_PREFIX "__co_"

// c++ ABI version
// TODO: condsider making this configurable in compiler_t
#define CO_LIBCXX_ABI_VERSION 1

// universe constants (universe.c)
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

// pkg
err_t pkg_init(pkg_t* pkg, memalloc_t ma);
void pkg_dispose(pkg_t* pkg, memalloc_t ma);
bool pkg_find_dir(pkg_t* pkg); // updates pkg->dir if successful
err_t pkgs_for_argv(int argc, char* argv[], pkg_t** pkgvp, u32* pkgcp);
srcfile_t* pkg_add_srcfile(pkg_t* pkg, const char* name);
err_t pkg_find_files(pkg_t* pkg); // updates pkg->files, sets sf.mtime
unixtime_t pkg_source_mtime(const pkg_t* pkg); // max(f.mtime for f in files)
bool pkg_is_built(const pkg_t* pkg, const compiler_t* c);
str_t pkg_builddir(const pkg_t* pkg, const compiler_t* c); // "{c.builddir}/{pkg.name}.copkg"
node_t* nullable pkg_def_get(pkg_t* pkg, sym_t name);
err_t pkg_def_set(pkg_t* pkg, memalloc_t ma, sym_t name, node_t* n);
err_t pkg_def_add(pkg_t* pkg, memalloc_t ma, sym_t name, node_t** np_inout);

// srcfile
err_t srcfile_open(srcfile_t* sf);
void srcfile_close(srcfile_t* sf);

// filetype
filetype_t filetype_guess(const char* filename);

// compiler
void compiler_init(compiler_t*, memalloc_t, diaghandler_t);
void compiler_dispose(compiler_t*);
err_t compiler_configure(compiler_t*, const compiler_config_t*);
err_t compile_c_to_obj_async(
  compiler_t* c, subprocs_t* sp, const char* wdir, const char* cfile, const char* ofile);
err_t compile_c_to_asm_async(
  compiler_t* c, subprocs_t* sp, const char* wdir, const char* cfile, const char* ofile);
bool compiler_fully_qualified_name(const compiler_t*, const pkg_t*, buf_t* dst, const node_t*);
bool compiler_mangle(const compiler_t*, const pkg_t*, buf_t* dst, const node_t*);
bool compiler_mangle_type(const compiler_t* c, const pkg_t*, buf_t* buf, const type_t* t);

// compiler_spawn_tool spawns a compiler subprocess in procs
err_t compiler_spawn_tool(
  const compiler_t* c, subprocs_t* procs, strlist_t* args, const char* nullable cwd);

// compiler_spawn_tool_p spawns a compiler subprocess at p
err_t compiler_spawn_tool_p(
  const compiler_t* c, subproc_t* p, strlist_t* args, const char* nullable cwd);

// compiler_run_tool_sync spawns a compiler subprocess and waits for it to complete
err_t compiler_run_tool_sync(
  const compiler_t* c, strlist_t* args, const char* nullable cwd);

// spawn_tool spawns a compiler subprocess
// argv must be NULL terminated; argv[0] must be the name of a compis command, eg "cc"
err_t spawn_tool(
  subproc_t* p, char*const* restrict argv, const char* restrict nullable cwd, int flags);
// flags for spawn_tool()
#define SPAWN_TOOL_NOFORK (1<<0) // prohibit use of fork()

// scanner
bool scanner_init(scanner_t* s, compiler_t* c);
void scanner_dispose(scanner_t* s);
void scanner_begin(scanner_t* s, srcfile_t*);
void scanner_next(scanner_t* s);
void stop_scanning(scanner_t* s);
slice_t scanner_lit(const scanner_t* s); // e.g. `"\n"` => slice_t{.chars="\n", .len=1}
slice_t scanner_strval(const scanner_t* s);

// parser
bool parser_init(parser_t* p, compiler_t* c);
void parser_dispose(parser_t* p);
err_t parser_parse(parser_t* p, memalloc_t ast_ma, srcfile_t*, unit_t** result);
inline static u32 parser_errcount(const parser_t* p) { return p->scanner.errcount; }

// typefuntab
err_t typefuntab_init(typefuntab_t* tfuns, memalloc_t ma);
void typefuntab_dispose(typefuntab_t* tfuns, memalloc_t ma);
fun_t* nullable typefuntab_lookup(typefuntab_t* tfuns, type_t* t, sym_t name);

// post-parse passes
err_t typecheck(compiler_t*, memalloc_t ast_ma, pkg_t* pkg, unit_t** unitv, u32 unitc);
err_t analyze(compiler_t* c, memalloc_t ast_ma, pkg_t* pkg, unit_t** unitv, u32 unitc);

// ir
bool irfmt(compiler_t*, const pkg_t*, buf_t*, const irunit_t*);
bool irfmt_dot(compiler_t*, const pkg_t*, buf_t*, const irunit_t*);
bool irfmt_fun(compiler_t*, const pkg_t*, buf_t*, const irfun_t*);

// C code generator
bool cgen_init(cgen_t* g, compiler_t* c, const pkg_t*, memalloc_t out_ma);
void cgen_dispose(cgen_t* g);
err_t cgen_generate(cgen_t* g, const unit_t* unit);

// AST
const char* nodekind_name(nodekind_t); // e.g. "EXPR_INTLIT"
const char* nodekind_fmt(nodekind_t); // e.g. "variable"
err_t node_fmt(buf_t* buf, const node_t* nullable n, u32 depth); // e.g. i32, x, "foo"
err_t node_repr(buf_t* buf, const node_t* n); // S-expr AST tree
origin_t node_origin(locmap_t*, const node_t*); // compute source origin of node
node_t* nullable ast_mknode(memalloc_t ast_ma, usize size, nodekind_t kind);
node_t* clone_node(parser_t* p, const node_t* n);
// T* CLONE_NODE(T* node)
#define CLONE_NODE(p, nptr) ( \
  (__typeof__(nptr))memcpy( \
    _mknode((p), sizeof(__typeof__(*(nptr))), ((node_t*)(nptr))->kind), \
    (nptr), \
    sizeof(*(nptr))) )
local_t* nullable lookup_struct_field(structtype_t* st, sym_t name);
fun_t* nullable lookup_method(parser_t* p, type_t* recv, sym_t name);
const char* node_srcfilename(const node_t* n, locmap_t* lm);

inline static void bubble_flags(void* parent, void* child) {
  ((node_t*)parent)->flags |= (((node_t*)child)->flags & NODEFLAGS_BUBBLE);
}

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
inline static bool nodekind_isreftype(nodekind_t kind) {
  return kind == TYPE_REF || kind == TYPE_MUTREF; }
inline static bool nodekind_isptrliketype(nodekind_t kind) {
  return kind == TYPE_PTR || kind == TYPE_REF || kind == TYPE_MUTREF; }
inline static bool nodekind_isslicetype(nodekind_t kind) {
  return kind == TYPE_SLICE || kind == TYPE_MUTSLICE; }
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

inline static bool type_isptr(const type_t* nullable t) {  // *T
  return nodekind_isptrtype(assertnotnull(t)->kind); }
inline static bool type_isref(const type_t* nullable t) {  // &T | mut&T
  return nodekind_isreftype(assertnotnull(t)->kind); }
inline static bool type_isptrlike(const type_t* nullable t) { // &T | mut&T | *T
  return nodekind_isptrliketype(assertnotnull(t)->kind); }
inline static bool type_isslice(const type_t* nullable t) {  // &[T] | mut&[T]
  return nodekind_isslicetype(assertnotnull(t)->kind); }
inline static bool type_isreflike(const type_t* nullable t) {
  // &T | mut&T | &[T] | mut&[T]
  return nodekind_isreftype(assertnotnull(t)->kind) ||
         nodekind_isslicetype(t->kind);
}
inline static bool type_isprim(const type_t* nullable t) {  // void, bool, int, u8, ...
  return nodekind_isprimtype(assertnotnull(t)->kind); }
inline static bool type_isopt(const type_t* nullable t) {  // ?T
  return assertnotnull(t)->kind == TYPE_OPTIONAL; }
inline static bool type_isbool(const type_t* nullable t) {  // bool
  return assertnotnull(t)->kind == TYPE_BOOL; }
bool type_isowner(const type_t* t);
inline static bool type_iscopyable(const type_t* t) {
  return !type_isowner(t);
}
inline static bool type_isunsigned(const type_t* t) {
  return TYPE_U8 <= t->kind && t->kind <= TYPE_UINT;
}
inline static bool funtype_hasthis(const funtype_t* ft) {
  return ft->params.len && ((local_t*)ft->params.v[0])->isthis;
}

sym_t nullable _typeid(type_t*);
inline static sym_t nullable typeid(type_t* t) { return t->tid ? t->tid : _typeid(t); }
#define TYPEID_PREFIX(typekind)  ('A'+((typekind)-TYPE_VOID))

inline static const type_t* canonical_primtype(const compiler_t* c, const type_t* t) {
  return t->kind == TYPE_INT ? c->inttype :
         t->kind == TYPE_UINT ? c->uinttype :
         t;
}

// type_unwrap_ptr unwraps optional, ref and ptr.
// e.g. "?&T" => "&T" => "T"
type_t* type_unwrap_ptr(type_t* t);

// expr_no_side_effects returns true if materializing n has no side effects
bool expr_no_side_effects(const expr_t* n);

// void assert_nodekind(const node_t* n, nodekind_t kind)
#define assert_nodekind(nodeptr, KIND) \
  assertf(((node_t*)(nodeptr))->kind == KIND, \
    "%s != %s", nodekind_name(((node_t*)(nodeptr))->kind), nodekind_name(KIND))

// asexpr(void* ptr) -> expr_t*
// asexpr(const void* ptr) -> const expr_t*
#define asexpr(ptr) ({ \
  __typeof__(ptr) ptr__ = (ptr); \
  assertf(node_isexpr((const node_t*)assertnotnull(ptr__)), "not an expression"), \
  _Generic(ptr__, \
    const node_t*: (const expr_t*)ptr__, \
    node_t*:       (expr_t*)ptr__, \
    const void*:   (const expr_t*)ptr__, \
    void*:         (expr_t*)ptr__ ); \
})


// comptime
typedef u8 ctimeflag_t;
#define CTIME_NO_DIAG ((ctimeflag_t)1<< 0) // do not report diagnostics
node_t* nullable comptime_eval(compiler_t*, expr_t*, ctimeflag_t); // NULL if OOM
bool comptime_eval_uint(compiler_t*, expr_t*, ctimeflag_t, u64* result);

// tokens
const char* tok_name(tok_t); // e.g. TEQ => "TEQ"
const char* tok_repr(tok_t); // e.g. TEQ => "="
usize tok_descr(char* buf, usize bufcap, tok_t, slice_t lit); // e.g. "number 3"
inline static bool tok_isassign(tok_t t) { return TASSIGN <= t && t <= TORASSIGN; }

// operations
const char* op_name(op_t); // e.g. OP_ADD => "OP_ADD"
const char* op_fmt(op_t);  // e.g. OP_ADD => "+"
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
inline static sym_t sym_cstr(const char* s) { return sym_intern(s, strlen(s)); }
extern sym_t _primtype_nametab[(TYPE_F64 - TYPE_VOID) + 1];
inline static sym_t primtype_name(nodekind_t kind) { // e.g. "i64"
  assert(TYPE_VOID <= kind && kind <= TYPE_F64);
  return _primtype_nametab[kind - TYPE_VOID];
}
extern sym_t sym__;    // "_"
extern sym_t sym_this; // "this"
extern sym_t sym_drop; // "drop"
extern sym_t sym_main; // "main"
extern sym_t sym_str;  // "str"
extern sym_t sym_void;
extern sym_t sym_bool;
extern sym_t sym_int;
extern sym_t sym_uint;
extern sym_t sym_i8;
extern sym_t sym_i16;
extern sym_t sym_i32;
extern sym_t sym_i64;
extern sym_t sym_u8;
extern sym_t sym_u16;
extern sym_t sym_u32;
extern sym_t sym_u64;
extern sym_t sym_f32;
extern sym_t sym_f64;

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

// temporary buffers
// get_tmpbuf returns a thread-local general-purpose temporary buffer
void tmpbuf_init(memalloc_t);
buf_t* tmpbuf_get(u32 bufindex /* [0-2) */);

// sysroot
#define SYSROOT_ENABLE_CXX (1<<0) // enable libc++, libc++abi, libunwind
err_t build_sysroot_if_needed(const compiler_t* c, int flags); // build_sysroot.c
const char* syslib_filename(const target_t* target, syslib_t);

// loc
err_t locmap_init(locmap_t* lm);
void locmap_dispose(locmap_t* lm, memalloc_t);
void locmap_clear(locmap_t* lm);
u32 locmap_srcfileid(locmap_t* lm, srcfile_t*, memalloc_t); // get id for source file

static loc_t loc_make(u32 inputid, u32 line, u32 col, u32 width);

srcfile_t* nullable loc_srcfile(loc_t p, locmap_t*);
static u32 loc_line(loc_t p);
static u32 loc_col(loc_t p);
static u32 loc_width(loc_t p);
static u32 loc_srcfileid(loc_t p); // key for locmap_t; 0 for pos without input

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
inline static bool loc_isknown(loc_t p) { return !!(loc_srcfileid(p) | loc_line(p)); }

// p is {before,after} q in same input
inline static bool loc_isbefore(loc_t p, loc_t q) { return p < q; }
inline static bool loc_isafter(loc_t p, loc_t q) { return p > q; }

// loc_fmt appends "file:line:col" to buf (behaves like snprintf)
usize loc_fmt(loc_t p, char* buf, usize bufcap, locmap_t* lm);

// origin_make creates a origin_t
// 1. origin_make(locmap_t*, loc_t)
// 2. origin_make(locmap_t*, loc_t, u32 focus_col)
#define origin_make(...) __VARG_DISP(_origin_make,__VA_ARGS__)
#define _origin_make1 _origin_make2 // catch "too few arguments to function call"
origin_t _origin_make2(locmap_t* m, loc_t loc);
origin_t _origin_make3(locmap_t* m, loc_t loc, u32 focus_col);

origin_t origin_union(origin_t a, origin_t b);


//—————————————————————————————————————————————————————
// loc implementation

// Limits: files: 1048575, lines: 1048575, columns: 4095, width: 4095
// If this is too tight, we can either make lico 64b wide, or we can introduce a
// tiered encoding where we remove column information as line numbers grow bigger.
static const u64 _loc_widthBits     = 12;
static const u64 _loc_colBits       = 12;
static const u64 _loc_lineBits      = 20;
static const u64 _loc_srcfileidBits = 64 - _loc_lineBits - _loc_colBits - _loc_widthBits;

static const u64 _loc_srcfileidMax = (1llu << _loc_srcfileidBits) - 1;
static const u64 _loc_lineMax      = (1llu << _loc_lineBits) - 1;
static const u64 _loc_colMax       = (1llu << _loc_colBits) - 1;
static const u64 _loc_widthMax     = (1llu << _loc_widthBits) - 1;

static const u64 _loc_srcfileidShift =
  _loc_srcfileidBits + _loc_colBits + _loc_widthBits;
static const u64 _loc_lineShift = _loc_colBits + _loc_widthBits;
static const u64 _loc_colShift  = _loc_widthBits;


inline static loc_t loc_make_unchecked(u32 inputid, u32 line, u32 col, u32 width) {
  return (loc_t)( ((loc_t)inputid << _loc_srcfileidShift)
              | ((loc_t)line << _loc_lineShift)
              | ((loc_t)col << _loc_colShift)
              | width );
}
inline static loc_t loc_make(u32 inputid, u32 line, u32 col, u32 width) {
  return loc_make_unchecked(
    MIN(_loc_srcfileidMax, inputid),
    MIN(_loc_lineMax, line),
    MIN(_loc_colMax, col),
    MIN(_loc_widthMax, width));
}
inline static u32 loc_srcfileid(loc_t p) { return p >> _loc_srcfileidShift; }
inline static u32 loc_line(loc_t p)    { return (p >> _loc_lineShift) & _loc_lineMax; }
inline static u32 loc_col(loc_t p)     { return (p >> _loc_colShift) & _loc_colMax; }
inline static u32 loc_width(loc_t p)   { return p & _loc_widthMax; }

// TODO: improve the efficiency of these
inline static loc_t loc_with_inputid(loc_t p, u32 inputid) {
  return loc_make_unchecked(
    MIN(_loc_srcfileidMax, inputid), loc_line(p), loc_col(p), loc_width(p));
}
inline static loc_t loc_with_line(loc_t p, u32 line) {
  return loc_make_unchecked(
    loc_srcfileid(p), MIN(_loc_lineMax, line), loc_col(p), loc_width(p));
}
inline static loc_t loc_with_col(loc_t p, u32 col) {
  return loc_make_unchecked(
    loc_srcfileid(p), loc_line(p), MIN(_loc_colMax, col), loc_width(p));
}
inline static loc_t loc_with_width(loc_t p, u32 width) {
  return loc_make_unchecked(
    loc_srcfileid(p), loc_line(p), loc_col(p), MIN(_loc_widthMax, width));
}

inline static void loc_set_line(loc_t* p, u32 line) { *p = loc_with_line(*p, line); }
inline static void loc_set_col(loc_t* p, u32 col) { *p = loc_with_col(*p, col); }
inline static void loc_set_width(loc_t* p, u32 width) { *p = loc_with_width(*p, width); }

inline static loc_t loc_min(loc_t a, loc_t b) {
  // pos-1 causes (loc_t)0 to become the maximum value of loc_t,
  // effectively preferring >(loc_t)0 over (loc_t)0 here.
  return (b-1 < a-1) ? b : a;
}
inline static loc_t loc_max(loc_t a, loc_t b) {
  return (b > a) ? b : a;
}



ASSUME_NONNULL_END
