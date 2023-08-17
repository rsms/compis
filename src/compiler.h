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
#include "future.h"
#include "sha256.h"
#include "tmpbuf.h"
#include "loc.h"
#include "srcfile.h"
#include "sym.h"
#include "tokens.h"
#include "diag.h"
#include "ast.h"
ASSUME_NONNULL_BEGIN

typedef u8 buildmode_t;
enum buildmode {
  BUILDMODE_DEBUG,
  BUILDMODE_OPT,
};

// compiler_t
typedef struct compiler_ {
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
  rwmutex_t      diag_mu;     // must hold lock when accessing the following fields
  diaghandler_t  diaghandler; // called when errors are encountered
  void* nullable userdata;    // passed to diaghandler
  _Atomic(u32)   errcount;    // number of errors encountered
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
  bool opt_nostdruntime : 1;
  u8   opt_verbose; // 0=off 1=on 2=extra

  // data created during parsing & analysis
  // mutex must be locked when multiple threads share a compiler instance
  locmap_t locmap; // maps loc_t => srcfile_t*

  // package index
  rwmutex_t       pkgindex_mu;    // guards access to pkgindex
  map_t           pkgindex;       // const char* abs_fspath -> pkg_t*
  pkg_t* nullable stdruntime_pkg; // std/runtime package
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
  bool nolibc;
  bool nolibcxx;
  bool nostdruntime; // do not include or link with std/runtime
  u8   verbose;

  // sysver sets the minimum system version. Ignored if NULL or "".
  // Currently only supported for macos via -mmacosx-version-min=sysver.
  // Defaults to the target's sysver if not set (common case.)
  const char* nullable sysver;

  // sysroot sets a custom sysroot. Ignored if NULL or "".
  const char* nullable sysroot;
} compiler_config_t;

typedef struct {
  u32    cap;  // capacity of ptr (in number of entries)
  u32    len;  // current length of ptr (entries currently stored)
  u32    base; // current scope's base index into ptr
  void** ptr;  // entries
} scope_t;

typedef struct {
  srcfile_t* srcfile;     // input source
  const u8*  inp;         // input buffer current pointer
  const u8*  inend;       // input buffer end
  const u8*  linestart;   // start of current line
  const u8*  tokstart;    // start of current token
  const u8*  tokend;      // end of previous token
  loc_t      loc;         // recently parsed token's source location
  tok_t      tok;         // recently parsed token (current token during scanning)
  bool       insertsemi;  // insert a semicolon before next newline
  u32        lineno;      // monotonic line number counter (!= tok.loc.line)
  u32        errcount;    // number of error diagnostics reported
  err_t      err;         // non-syntax error that occurred

  // u16  indent;           // current level
  u32  indentdst;        // unwind to level
  u32  indentstackv[32]; // previous indentation levels
  u32* indentstack;      // top of indentstackv

  // bool insertsemi2;
  // tok_t tokq[64];
  // u8    tokqlen;
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

  // free_nodearrays is a free list of nodearray_t's
  struct {
    nodearray_t* nullable v;
    u32                   len;
    u32                   cap;
  } free_nodearrays;

  #if DEBUG
    int traceindent;
  #endif
} parser_t;

#define CGEN_EXE     (1u << 0) // generating code for an executable
#define CGEN_SRCINFO (1u << 1) // generate `#line N "source.co"`

typedef struct {
  compiler_t*  compiler;
  const pkg_t* pkg;
  memalloc_t   ma;         // compiler->ma
  buf_t        outbuf;
  u32          flags;      // CGEN_*
  u32          srcfileid;
  u32          lineno;
  u32          scopenest;
  err_t        err;
  u32          idgen_local;
  usize        indent;
  map_t        typedefmap;
  map_t        tmpmap;
  const fun_t* nullable mainfun;
  #ifdef DEBUG
  int traceindent;
  #endif
} cgen_t;

typedef struct {
  slice_t pub_header;   // .h file data of public statements (ref cgen_t.outbuf)
  str_t   pkg_header;   // statements for all units of the package
  map_t   pkg_typedefs; // type definitions for all PKG- & PUB-visibility interfaces
  // note: pkgapidata and pkgtypedefs are allocated in cgen_t.ma, it's the
  // responsibility of the cgen_pkgapi caller to free these with cgen_pkgapi_dispose.
  nodearray_t defs;
} cgen_pkgapi_t;


extern node_t* last_resort_node;

// name prefix reserved for implementation
//
// Caution! Changing any of this alters the ABI; invalidates exising generated code.
// Node: coprelude.h has these values hard-coded and needs manual updating.
#define CO_MANGLE_PREFIX     "_co"
#define CO_ABI_GLOBAL_PREFIX "__co_"
// mangledname of built-ins
#define CO_MANGLEDNAME_STR  CO_ABI_GLOBAL_PREFIX "str"
#define CO_MANGLEDNAME_INT  CO_ABI_GLOBAL_PREFIX "int"
#define CO_MANGLEDNAME_UINT CO_ABI_GLOBAL_PREFIX "uint"

// c++ ABI version
// TODO: condsider making this configurable in compiler_t
#define CO_LIBCXX_ABI_VERSION 1

// universe
extern type_t* type_void;
extern type_t* type_bool;
extern type_t* type_i8;
extern type_t* type_i16;
extern type_t* type_i32;
extern type_t* type_i64;
extern type_t* type_int;
extern type_t* type_u8;
extern type_t* type_u16;
extern type_t* type_u32;
extern type_t* type_u64;
extern type_t* type_uint;
extern type_t* type_f32;
extern type_t* type_f64;
extern type_t* type_unknown;
void universe_init();


// compiler
void compiler_init(compiler_t*, memalloc_t, diaghandler_t);
void compiler_dispose(compiler_t*);
err_t compiler_configure(compiler_t*, const compiler_config_t*);
err_t compile_c_to_obj_async(
  compiler_t* c, subprocs_t* sp, const char* wdir, const char* cfile, const char* ofile);
err_t compile_c_to_asm_async(
  compiler_t* c, subprocs_t* sp, const char* wdir, const char* cfile, const char* ofile);
bool compiler_fully_qualified_name(
  const compiler_t*, const pkg_t*, buf_t* dst, const node_t*);
bool compiler_mangle(const compiler_t*, const pkg_t*, buf_t* dst, const node_t*);
bool compiler_mangle_type(
  const compiler_t* c, const pkg_t*, buf_t* buf, const type_t* t);

node_t* clone_node(parser_t* p, const node_t* n);
fun_t* nullable lookup_method(parser_t* p, type_t* recv, sym_t name);

inline static const type_t* canonical_primtype(const compiler_t* c, const type_t* t) {
  return t->kind == TYPE_INT ? c->inttype :
         t->kind == TYPE_UINT ? c->uinttype :
         t;
}

// mangle_str writes str to buf, escaping any chars not in 0-9A-Za-z_
// by "$XX" where XX is the hexadecimal encoding of the byte value,
// or "·" (U+00B7, UTF-8 C2 B7) for '/' and '\'
bool mangle_str(buf_t* buf, slice_t str);

// compiler_spawn_tool spawns a compiler subprocess in procs
err_t compiler_spawn_tool(
  const compiler_t* c, subprocs_t* procs, strlist_t* args, const char* nullable cwd);

// compiler_spawn_tool_p spawns a compiler subprocess at p
err_t compiler_spawn_tool_p(
  const compiler_t* c, subproc_t* p, strlist_t* args, const char* nullable cwd);

// compiler_run_tool_sync spawns a compiler subprocess and waits for it to complete
err_t compiler_run_tool_sync(
  const compiler_t* c, strlist_t* args, const char* nullable cwd);

// compiler_errcount retrieves the number of DIAG_ERR diagnostics produced so far
inline static u32 compiler_errcount(const compiler_t* c) {
  return AtomicLoadAcq(&c->errcount);
}

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

// post-parse passes
err_t typecheck(compiler_t*, memalloc_t ast_ma, pkg_t* pkg, unit_t** unitv, u32 unitc);
err_t iranalyze(compiler_t*, memalloc_t ast_ma, pkg_t* pkg, unit_t** unitv, u32 unitc);
err_t check_typedeps(compiler_t* c, unit_t** unitv, u32 unitc);
bool check_typedep(compiler_t* c, node_t* n);

// importing of packages
bool import_validate_path(const char* path, const char** errmsgp, usize* erroffsp);
err_t import_pkgs(compiler_t* c, pkg_t* importer_pkg, unit_t* unitv[], u32 unitc);
err_t import_resolve_pkg(
  compiler_t* c,
  const pkg_t* importer_pkg,
  slice_t path,
  str_t* fspath,
  pkg_t** result);
err_t import_resolve_fspath(str_t* fspath, usize* rootlen_out);

err_t pkgindex_intern(
  compiler_t* c,
  slice_t pkgdir,
  slice_t pkgpath,
  const sha256_t* nullable api_sha256,
  pkg_t** result);
err_t pkgindex_add(compiler_t* c, pkg_t* pkg);

// C code generator
bool cgen_init(
  cgen_t* g, compiler_t* c, const pkg_t*, memalloc_t out_ma, u32 flags);
void cgen_dispose(cgen_t* g);
err_t cgen_unit_impl(cgen_t* g, unit_t* unit, cgen_pkgapi_t* pkgapi);
err_t cgen_pkgapi(cgen_t* g, unit_t** unitv, u32 unitc, cgen_pkgapi_t* result);
void cgen_pkgapi_dispose(cgen_t* g, cgen_pkgapi_t* result);

// co_strlit_check validates a compis string literal while calculating
// its decoded byte length. I.e. "hello\nworld" is 11 bytes decoded.
// src should point to a string starting with '"'.
//
// Returns 0 if the string is valid, sets *enclenp to #bytes read from src and
// sets *declenp to the length of the decoded string. I.e.
//
//   const char* src = "\"hello\nworld\"yo"
//   usize srclen = strlen(src), declen;
//   co_strlit_check(src, &srclen, &declen);
//   printf("srclen=%zu declen=%zu src_tail=%s\n", src + srclen);
//   // prints: srclen=14 declen=11 src_tail=yo
//
// If an error is returned, srcp is updated to point to the offending byte,
// which may be *srcp+srclen if there was no terminating '"'.
err_t co_strlit_check(const u8* src, usize* srclenp, usize* declenp);

// co_strlit_decode decodes a string literal with input from co_strlit_check.
// Example:
//
//   usize srclen = ... , declen;
//   co_strlit_check(src, &srclen, &declen); // + check error
//   u8* dst = mem_alloc(ma, declen);        // + check error
//   if (enclen - 2 == declen) {
//     memcpy(dst, src + 1, declen);
//   } else {
//     co_strlit_decode(src, enclen, dst, declen); // + check error
//   }
//
err_t co_strlit_decode(const u8* src, usize srclen, u8* dst, usize declen);

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
u32 scope_level(const scope_t* nullable s);
typedef bool(*scopeit_t)(const void* key, const void* value, void* nullable ctx);
void scope_iterate(scope_t* s, u32 maxdepth, scopeit_t it, void* nullable ctx);

// visibility
const char* visibility_str(nodeflag_t flags);

// sysroot
#define SYSROOT_ENABLE_CXX (1<<0) // enable libc++, libc++abi, libunwind
err_t build_sysroot_if_needed(const compiler_t* c, int flags); // build_sysroot.c
const char* syslib_filename(const target_t* target, syslib_t);


ASSUME_NONNULL_END
