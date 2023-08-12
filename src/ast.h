// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "srcfile.h"
#include "loc.h"
#include "sym.h"
#include "array.h"
#include "buf.h"
#include "future.h"
#include "map.h"
#include "sha256.h"
#include "str.h"
#include "thread.h"
#include "ops.h"

typedef u8 nodekind_t;
#define FOREACH_NODEKIND_NODE(_) /* nodekind_t, TYPE, enctag */ \
  _( NODE_BAD,      node_t,          'BAD ')/* invalid node; parse error */ \
  _( NODE_COMMENT,  node_t,          'COMN')\
  _( NODE_UNIT,     unit_t,          'UNIT')\
  _( NODE_IMPORTID, importid_t,      'IMID')\
  _( NODE_TPLPARAM, templateparam_t, 'TPAR')\
  _( NODE_FWDDECL,  fwddecl_t,       'FDCL')\
// end FOREACH_NODEKIND
#define FOREACH_NODEKIND_STMT(_) /* nodekind_t, TYPE, enctag */ \
  /* statements */\
  _( STMT_TYPEDEF, typedef_t, 'TDEF')\
  _( STMT_IMPORT,  import_t,  'IMP ')\
// end FOREACH_NODEKIND_STMT
#define FOREACH_NODEKIND_EXPR(_) /* nodekind_t, TYPE, enctag */ \
  /* expressions */\
  _( EXPR_FUN,       fun_t,       'FUN ')/* nodekind_isexpr expects position */\
  _( EXPR_BLOCK,     block_t,     'BLK ')\
  _( EXPR_CALL,      call_t,      'CALL')\
  _( EXPR_TYPECONS,  typecons_t,  'TCON')\
  _( EXPR_ID,        idexpr_t,    'ID  ')\
  _( EXPR_NS,        nsexpr_t,    'NS  ')\
  _( EXPR_FIELD,     local_t,     'FILD')\
  _( EXPR_PARAM,     local_t,     'PARM')\
  _( EXPR_VAR,       local_t,     'VAR ')\
  _( EXPR_LET,       local_t,     'LET ')\
  _( EXPR_MEMBER,    member_t,    'MEMB')\
  _( EXPR_SUBSCRIPT, subscript_t, 'SUBS')\
  _( EXPR_PREFIXOP,  unaryop_t,   'PREO')\
  _( EXPR_POSTFIXOP, unaryop_t,   'POSO')\
  _( EXPR_DEREF,     unaryop_t,   'DREF')/* implicit read of &T (expl=EXPR_PREFIXOP) */\
  _( EXPR_BINOP,     binop_t,     'BINO')\
  _( EXPR_ASSIGN,    binop_t,     'ASGN')\
  _( EXPR_IF,        ifexpr_t,    'IF  ')\
  _( EXPR_FOR,       forexpr_t,   'FOR ')\
  _( EXPR_RETURN,    retexpr_t,   'RET ')\
  _( EXPR_BOOLLIT,   intlit_t,    'BLIT')\
  _( EXPR_INTLIT,    intlit_t,    'ILIT')\
  _( EXPR_FLOATLIT,  floatlit_t,  'FLIT')\
  _( EXPR_STRLIT,    strlit_t,    'SLIT')\
  _( EXPR_ARRAYLIT,  arraylit_t,  'ALIT')\
// end FOREACH_NODEKIND_EXPR
#define FOREACH_NODEKIND_PRIMTYPE(_) /* nodekind_t, TYPE, enctag, NAME, size */\
  /* primitive types (TYPE is always type_t) */\
  _( TYPE_VOID,    type_t, 'void', void, 0 )/* must be first type kind */\
  _( TYPE_BOOL,    type_t, 'bool', bool, 1 )\
  _( TYPE_I8,      type_t, 'i8  ', i8,   1 )\
  _( TYPE_I16,     type_t, 'i16 ', i16,  2 )\
  _( TYPE_I32,     type_t, 'i32 ', i32,  4 )\
  _( TYPE_I64,     type_t, 'i64 ', i64,  8 )\
  _( TYPE_INT,     type_t, 'int ', int,  4 )\
  _( TYPE_U8,      type_t, 'u8  ', u8,   1 )\
  _( TYPE_U16,     type_t, 'u16 ', u16,  2 )\
  _( TYPE_U32,     type_t, 'u32 ', u32,  4 )\
  _( TYPE_U64,     type_t, 'u64 ', u64,  8 )\
  _( TYPE_UINT,    type_t, 'uint', uint, 4 )\
  _( TYPE_F32,     type_t, 'f32 ', f32,  4 )\
  _( TYPE_F64,     type_t, 'f64 ', f64,  8 )\
  _( TYPE_UNKNOWN, type_t, 'unkn', unknown, 0 )\
// end FOREACH_NODEKIND_PRIMTYPE
#define FOREACH_NODEKIND_USERTYPE(_) /* nodekind_t, TYPE, enctag */\
  /* user types */\
  _( TYPE_ARRAY,    arraytype_t,  'arry')/* nodekind_is*type expects position */\
  _( TYPE_FUN,      funtype_t,    'fun ')\
  _( TYPE_PTR,      ptrtype_t,    'ptr ')\
  _( TYPE_REF,      reftype_t,    'ref ')/* &T      */\
  _( TYPE_MUTREF,   reftype_t,    'mref')/* mut&T   */\
  _( TYPE_SLICE,    slicetype_t,  'slc ')/* &[T]    */\
  _( TYPE_MUTSLICE, slicetype_t,  'mslc')/* mut&[T] */\
  _( TYPE_OPTIONAL, opttype_t,    'opt ')/* ?T */\
  _( TYPE_STRUCT,   structtype_t, 'st  ')\
  _( TYPE_ALIAS,    aliastype_t,  'alis')\
  _( TYPE_NS,       nstype_t,     'ns  ')\
  /* special types replaced by typecheck */\
  _( TYPE_TEMPLATE,    templatetype_t,    'tpl ')/* template instance */ \
  _( TYPE_PLACEHOLDER, placeholdertype_t, 'plac')/* template parameter */ \
  _( TYPE_UNRESOLVED,  unresolvedtype_t,  'ures')/* named type not yet resolved */ \
// end FOREACH_NODEKIND_USERTYPE

#define FOREACH_NODEKIND(_) /* _(nodekind, TYPE, enctag, ...) */ \
  FOREACH_NODEKIND_NODE(_) \
  FOREACH_NODEKIND_STMT(_) \
  FOREACH_NODEKIND_EXPR(_) \
  FOREACH_NODEKIND_PRIMTYPE(_) \
  FOREACH_NODEKIND_USERTYPE(_)

enum nodekind {
  #define _(NAME, ...) NAME,
  FOREACH_NODEKIND(_)
  #undef _
};
enum { NODEKIND_COUNT = (0lu FOREACH_NODEKIND(CO_PLUS_ONE) ) };
enum { PRIMTYPE_COUNT = (0lu FOREACH_NODEKIND_PRIMTYPE(CO_PLUS_ONE) ) };

// AST node flags
typedef u16 nodeflag_t;
#define NF_VIS_MASK    ((nodeflag_t)3)      // 0b11
#define NF_VIS_UNIT    ((nodeflag_t)0)      // visible within same source file
#define NF_VIS_PKG     ((nodeflag_t)1<< 0)  // visible within same package
#define NF_VIS_PUB     ((nodeflag_t)1<< 1)  // visible to other packages
#define NF_CHECKED     ((nodeflag_t)1<< 2)  // has been typecheck'ed (or doesn't need it)
#define NF_RVALUE      ((nodeflag_t)1<< 3)  // expression is used as an rvalue
#define NF_NARROWED    ((nodeflag_t)1<< 4)  // type-narrowed from optional
#define NF_UNKNOWN     ((nodeflag_t)1<< 5)  // has or contains unresolved identifier
#define NF_NAMEDPARAMS ((nodeflag_t)1<< 6)  // function has named parameters
#define NF_DROP        ((nodeflag_t)1<< 7)  // type has drop() function
#define NF_SUBOWNERS   ((nodeflag_t)1<< 8)  // type has owning elements
#define NF_EXIT        ((nodeflag_t)1<< 9)  // block exits (i.e. "return" or "break")
#define NF_CONST       ((nodeflag_t)1<< 9)  // [anything but block] is a constant
#define NF_PKGNS       ((nodeflag_t)1<< 10) // [namespace] is a package API
#define NF_TEMPLATE    ((nodeflag_t)1<< 11) // templatized with templateparam_t
#define NF_TEMPLATEI   ((nodeflag_t)1<< 12) // instance of template
#define NF_CYCLIC      ((nodeflag_t)1<< 13) // [usertype] references itself
#define NF_MARK1       ((nodeflag_t)1<< 14) // general-use marker
#define NF_MARK2       ((nodeflag_t)1<< 15) // general-use marker

typedef nodeflag_t nodevis_t; // symbolic
static_assert(0 < NF_VIS_PKG, "");
static_assert(NF_VIS_PKG < NF_VIS_PUB, "");

// NODEFLAGS_BUBBLE are flags that "bubble" (transfer) from children to parents
#define NODEFLAGS_BUBBLE  NF_UNKNOWN

// NODEFLAGS_TYPEID_MASK: flags included in typeid
#define NODEFLAGS_TYPEID_MASK NF_VIS_MASK \
                            | NF_NARROWED \
                            | NF_TEMPLATE \
                            | NF_TEMPLATEI \
// end NODEFLAGS_TYPEID_MASK

typedef const u8* typeid_t;


ASSUME_NONNULL_BEGIN

typedef struct node_ node_t;
typedef struct nsexpr_ nsexpr_t;
typedef struct fun_ fun_t;

typedef array_type(node_t*) nodearray_t; // cap==len
DEF_ARRAY_TYPE_API(node_t*, nodearray)

// typefuntab_t maps types to sets of type functions.
// Each pkg_t has a typefuntab_t describing type functions defined by that package.
// Each unit_t has a typefuntab_t describing imported type functions.
typedef struct {
  map_t     m;  // { sym_t typeid => map_t*{ sym_t name => fun_t* } }
  rwmutex_t mu; // guards access to m
} typefuntab_t;

// pkg_t represents a package
typedef struct pkg_ {
  str_t           path;     // import path, e.g. "main" or "std/runtime" (canonical)
  str_t           dir;      // absolute path to source directory
  str_t           root;     // root + path = dir
  bool            isadhoc;  // single-file package
  ptrarray_t      srcfiles; // source files, uniquely sorted by name
  map_t           defs;     // package-level definitions
  rwmutex_t       defs_mu;  // protects access to defs field
  typefuntab_t    tfundefs; // type functions defined by the package
  fun_t* nullable mainfun;  // fun main(), if any
  ptrarray_t      imports;  // pkg_t*[] -- imported packages (set by import_pkgs)
  sha256_t        api_sha256; // SHA-256 sum of pub.h

  future_t           loadfut;
  nodearray_t        api;     // package-level declarations, available after loadfut
  nsexpr_t* nullable api_ns;  // set by pkgbuild after loading api
  unixtime_t         mtime;
} pkg_t;

#define PKG_METAFILE_NAME "pub.coast"
#define PKG_APIHFILE_NAME "pub.h"


//———————————————————————————————————————————————————————————————————————————————————————
// AST node structs

typedef struct node_ {
  nodekind_t kind;
  u8         _unused;
  nodeflag_t flags;
  u32        nuse; // number of uses (expr_t and usertype_t)
  loc_t      loc;
} node_t;

typedef struct {
  node_t;
  node_t* decl;
} fwddecl_t;

typedef struct {
  node_t;
} stmt_t;

typedef struct import_t import_t;
typedef struct importid_t importid_t;

typedef struct {
  node_t;
  nodearray_t         children;
  srcfile_t* nullable srcfile;
  typefuntab_t        tfuns;      // imported type functions
  import_t* nullable  importlist; // list head
} unit_t;

typedef struct type_ {
  node_t;
  u64               size;
  u8                align;
  typeid_t nullable _typeid;
} type_t;

typedef struct {
  stmt_t;
  type_t* nullable type;
} expr_t;

typedef struct import_t {
  stmt_t;                           // .loc is location of "import" keyword
  char*                path;        // e.g. "foo/lolcat"
  loc_t                pathloc;     // source location of path
  sym_t                name;        // package identifier (sym__ if pkg is not imported)
  loc_t                nameloc;     // source location of name
  importid_t* nullable idlist;      // imported identifiers (list head)
  pkg_t* nullable      pkg;         // resolved package (set by import_pkgs)
  import_t* nullable   next_import; // linked-list link
} import_t;

typedef struct importid_t {
  node_t;
  loc_t                orignameloc; // location of "y" in "y as x"
  sym_t                name;        // e.g. x in "import x from a" (sym__ = "*")
  sym_t nullable       origname;    // e.g. y in "import y as x from a"
  importid_t* nullable next_id;     // linked-list link
} importid_t;

typedef struct {
  stmt_t;
  type_t* type; // TYPE_STRUCT or TYPE_ALIAS
} typedef_t;

typedef struct templateparam_ templateparam_t;
typedef struct templateparam_ {
  node_t;
  sym_t            name;
  node_t* nullable init; // e.g. "y" in "x = y"
  templateparam_t* nullable next_templateparam;
} templateparam_t;

typedef struct {
  type_t;
  // templateparams
  // If flags&NF_TEMPLATE: list of templateparam_t*
  // If flags&NF_TEMPLATEI: list of parameter arguments (node_t*)
  // Note: NF_TEMPLATEI is set on instances of templatetype_t.
  nodearray_t    templateparams;
  char* nullable mangledname; // allocated in ast_ma
} usertype_t;

typedef struct {
  usertype_t;
  sym_t            name;
  type_t* nullable resolved; // used by typecheck
} unresolvedtype_t;

typedef struct {
  usertype_t;
  templateparam_t* templateparam;
} placeholdertype_t;

// templatetype_t is an _instantation_ of a template.
// Template _definitions_ are denoted by NF_TEMPLATE
// with params at usertype_t.templateparams
typedef struct {
  usertype_t;         // loc is opening "<"
  loc_t       endloc; // ">"
  usertype_t* recv;
  nodearray_t args;   // type_t*[]
} templatetype_t;

typedef struct {
  usertype_t;
  nodearray_t members;
} nstype_t;

typedef struct { // *T
  usertype_t;
  type_t* elem;
} ptrtype_t;

typedef struct { // type A B
  ptrtype_t;
  sym_t            name;
  node_t* nullable nsparent;    // TODO: generalize to just "parent"
} aliastype_t;

typedef struct { // ?T
  ptrtype_t;
} opttype_t;

typedef struct { // &T, mut&T
  ptrtype_t;
} reftype_t;

typedef struct { // &[T], mut&[T]
  ptrtype_t;
  loc_t endloc; // "]"
} slicetype_t;

typedef struct { // [T]
  ptrtype_t;
  loc_t            endloc; // "]"
  u64              len;
  expr_t* nullable lenexpr;
} arraytype_t;

typedef struct {
  usertype_t;
  type_t*     result;
  nodearray_t params;       // local_t*[]
  loc_t       paramsloc;    // location of "(" ...
  loc_t       paramsendloc; // location of ")"
  loc_t       resultloc;    // location of result
} funtype_t;

typedef struct {
  usertype_t;
  sym_t nullable   name;     // NULL if anonymous
  nodearray_t      fields;   // local_t*[]
  node_t* nullable nsparent; // TODO: generalize to just "parent"
  bool             hasinit;  // true if at least one field has an initializer
  // TODO: move hasinit to nodeflag_t
} structtype_t;

typedef struct {
  sym_t   name;
  type_t* type;
} drop_t;

typedef array_type(drop_t) droparray_t;
DEF_ARRAY_TYPE_API(drop_t, droparray)

typedef struct { expr_t; u64 intval; } intlit_t;
typedef struct { expr_t; double f64val; } floatlit_t;
typedef struct { expr_t; u8* bytes; u64 len; } strlit_t;
typedef struct { expr_t; loc_t endloc; nodearray_t values; } arraylit_t;
typedef struct { expr_t; sym_t name; node_t* nullable ref; } idexpr_t;
typedef struct { expr_t; op_t op; expr_t* expr; } unaryop_t;
typedef struct { expr_t; op_t op; expr_t* left; expr_t* right; } binop_t;
typedef struct { expr_t; expr_t* nullable value; } retexpr_t;

typedef struct nsexpr_ {
  expr_t;
  union {
    sym_t  name; // if not NF_PKGNS
    pkg_t* pkg;  // if NF_PKGNS
  };
  nodearray_t members;      // node_t*[]
  sym_t*      member_names; // index in sync with members array
} nsexpr_t;

typedef struct {
  expr_t;
  expr_t*     recv;
  nodearray_t args; // expr_t*[] (local_t/EXPR_PARAM if named)
  loc_t       argsendloc; // location of ")"
} call_t;

typedef struct {
  expr_t;
  union {
    expr_t* nullable expr; // argument for primitive types
    nodearray_t      args; // arguments for all other types
  };
} typecons_t;

typedef struct { // block is a declaration (stmt) or an expression depending on use
  expr_t;
  nodearray_t children;
  droparray_t drops;    // drop_t[]
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

typedef u8 abi_t;
enum abi {
  ABI_CO = 0,
  ABI_C  = 1,
};

typedef struct fun_ { // fun is a declaration (stmt) or an expression depending on use
  expr_t;
  sym_t nullable    name;         // NULL if anonymous
  loc_t             nameloc;      // source location of name
  block_t* nullable body;         // NULL if function is a prototype
  type_t* nullable  recvt;        // non-NULL for type functions (type of "this")
  char* nullable    mangledname;  // mangled name, created in ast_ma
  loc_t             paramsloc;    // location of "(" ...
  loc_t             paramsendloc; // location of ")"
  loc_t             resultloc;    // location of result
  abi_t             abi;      // TODO: move to nodeflag_t
  node_t* nullable  nsparent; // TODO: generalize to just "parent"
} fun_t;


//———————————————————————————————————————————————————————————————————————————————————————

typedef struct compiler_ compiler_t;

//———————————————————————————————————————————————————————————————————————————————————————
// functions


const char* nodekind_name(nodekind_t); // e.g. "EXPR_INTLIT"
const char* nodekind_fmt(nodekind_t); // e.g. "variable"
err_t node_fmt(buf_t* buf, const node_t* nullable n, u32 depth); // e.g. i32, x, "foo"
err_t ast_repr(buf_t* buf, const node_t* n); // S-expr AST tree
err_t ast_repr_pkg(buf_t* buf, const pkg_t* pkg, const unit_t*const* unitv, u32 unitc);
node_t* nullable ast_mknode(memalloc_t ast_ma, usize size, nodekind_t kind);
bool ast_is_main_fun(const fun_t* fn);
local_t* nullable lookup_struct_field(structtype_t* st, sym_t name);
const char* node_srcfilename(const node_t* n, locmap_t* lm);


inline static void bubble_flags(void* parent, void* child) {
  ((node_t*)parent)->flags |= (((node_t*)child)->flags & NODEFLAGS_BUBBLE);
}

inline static void node_upgrade_visibility(node_t* n, nodeflag_t minvis) {
  assertf(minvis == NF_VIS_UNIT ||
          (NF_VIS_PKG <= minvis && minvis <= NF_VIS_PUB), "%x", minvis);
  if ((n->flags & NF_VIS_MASK) < minvis)
    n->flags = (n->flags & ~NF_VIS_MASK) | minvis;
}

inline static void node_set_visibility(node_t* n, nodeflag_t vis) {
  assertf(vis == NF_VIS_UNIT || (NF_VIS_PKG <= vis && vis <= NF_VIS_PUB), "%x", vis);
  n->flags = (n->flags & ~NF_VIS_MASK) | vis;
}

inline static bool nodekind_istype(nodekind_t kind) { return kind >= TYPE_VOID; }
inline static bool nodekind_isexpr(nodekind_t kind) {
  return EXPR_FUN <= kind && kind < TYPE_VOID; }
inline static bool nodekind_islocal(nodekind_t kind) {
  return kind == EXPR_FIELD || kind == EXPR_PARAM
      || kind == EXPR_LET   || kind == EXPR_VAR; }
inline static bool nodekind_isprimtype(nodekind_t kind) {
  return TYPE_VOID <= kind && kind <= TYPE_UNKNOWN; }
inline static bool nodekind_isusertype(nodekind_t kind) {
  return TYPE_ARRAY <= kind && kind <= TYPE_UNRESOLVED; }
inline static bool nodekind_isptrtype(nodekind_t kind) { return kind == TYPE_PTR; }
inline static bool nodekind_isreftype(nodekind_t kind) {
  return kind == TYPE_REF || kind == TYPE_MUTREF; }
inline static bool nodekind_isptrliketype(nodekind_t kind) {
  return kind == TYPE_PTR
      || kind == TYPE_REF
      || kind == TYPE_MUTREF; }
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

// type_unwrap_ptr unwraps optional, ref and ptr.
// e.g. "?&T" => "&T" => "T"
type_t* type_unwrap_ptr(type_t* t);

// expr_no_side_effects returns true if materializing n has no side effects.
// I.e. if removing n has no effect on the semantic of any other code outside it.
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


// ast_origin returns the source origin of an AST node
origin_t ast_origin(locmap_t*, const node_t*);


// pkg
err_t pkg_init(pkg_t* pkg, memalloc_t ma);
void pkg_dispose(pkg_t* pkg, memalloc_t ma);
err_t pkgs_for_argv(int argc, const char*const argv[], pkg_t** pkgvp, u32* pkgcp);
srcfile_t* nullable pkg_add_srcfile(
  pkg_t* pkg, const char* name, usize namelen, bool* nullable added_out);
err_t pkg_find_files(pkg_t* pkg); // updates pkg->files, sets sf.mtime
unixtime_t pkg_source_mtime(const pkg_t* pkg); // max(f.mtime for f in files)
bool pkg_is_built(const pkg_t* pkg, const compiler_t* c);
bool pkg_dir_of_root_and_path(str_t* dst, slice_t root, slice_t path);

// exe: "{builddir}/bin/{basename(pkg.path)}"
// lib: "{pkgbuilddir}/lib{basename(pkg.path)}.a"
bool pkg_builddir(const pkg_t* pkg, const compiler_t* c, str_t* dst);
bool pkg_buildfile(
  const pkg_t* pkg, const compiler_t* c, str_t* dst, const char* filename);
bool pkg_libfile(const pkg_t* pkg, const compiler_t* c, str_t* dst);
bool pkg_exefile(const pkg_t* pkg, const compiler_t* c, str_t* dst);

node_t* nullable pkg_def_get(pkg_t* pkg, sym_t name);
err_t pkg_def_set(pkg_t* pkg, memalloc_t ma, sym_t name, node_t* n);
err_t pkg_def_add(pkg_t* pkg, memalloc_t ma, sym_t name, node_t** np_inout);
str_t pkg_unit_srcdir(const pkg_t* pkg, const unit_t* unit);
// pkg_imports_add adds dep to importer_pkg->imports (uniquely)
bool pkg_imports_add(pkg_t* importer_pkg, pkg_t* dep, memalloc_t ma);


// typefuntab
err_t typefuntab_init(typefuntab_t* tfuns, memalloc_t ma);
void typefuntab_dispose(typefuntab_t* tfuns, memalloc_t ma);
fun_t* nullable typefuntab_lookup(typefuntab_t* tfuns, type_t* t, sym_t name);


// iterator
typedef struct { uintptr opaque[3]; } ast_childit_t;
ast_childit_t ast_childit(node_t* n);
node_t** nullable ast_childit_next(ast_childit_t* it); // NULL on "end of iteration"
ast_childit_t ast_childit_const(const node_t* n);
const node_t* nullable ast_childit_const_next(ast_childit_t* it);


// ast_clone_node creates a shallow copy of n, allocated in ma.
// note: fields of array_type have their underlying arrays copied as well.
// Returns NULL if memory allocation failed.
//
// T ast_clone_node<T is node_t*>(memalloc_t ma, const T srcnode)
#define ast_clone_node(ma, srcnodep) \
  ( (__typeof__(*(srcnodep))*)_ast_clone_node((ma), (srcnodep)) )
node_t* nullable _ast_clone_node(memalloc_t ma, const void* srcnodep);


// transformer
// ast_transformer_t can return NULL to stop transformation
typedef struct ast_transform_ ast_transform_t;

typedef node_t* nullable(*ast_transformer_t)(
  ast_transform_t* tr, node_t* n, void* nullable ctx);

err_t ast_transform(
  node_t*           n,
  memalloc_t        ast_ma,
  ast_transformer_t trfn,
  void* nullable    ctx,
  node_t**          result);

node_t* nullable ast_transform_children(
  ast_transform_t* tr, node_t* n, void* nullable ctx);


// ast_toposort_visit_def appends to defs n and all dependencies of n,
// in topological order (dependencies first, dependants after.)
// When a dependency cycle is detected, a fwddecl_t(NODE_FWDDECL) node is inserted,
// pointing to a dependency (inserted before the first dependant.)
// If visibility>0, only nodes with one of the provided visibility flags are included.
bool ast_toposort_visit_def(
  nodearray_t* defs, memalloc_t ma, nodeflag_t visibility, node_t* n);


// funtype_params_origin returns the origin of parameters, e.g.
//   fun foo(x, y int) int
//          ~~~~~~~~~~
origin_t fun_params_origin(locmap_t*, const fun_t* fn);
origin_t funtype_params_origin(locmap_t*, const funtype_t* ft);


// _sym_primtype_nametab is defined in sym.c, initialized by sym_init
extern sym_t _sym_primtype_nametab[PRIMTYPE_COUNT];

inline static sym_t primtype_name(nodekind_t kind) { // e.g. "i64"
  assert(TYPE_VOID <= kind && kind <= TYPE_UNKNOWN);
  return _sym_primtype_nametab[kind - TYPE_VOID];
}


static typeid_t typeid_intern(type_t* t);
static typeid_t typeid_of(const type_t* t);
void typeid_init(memalloc_t);
static u32 typeid_len(typeid_t);

typeid_t _typeid(type_t*, bool intern);
inline static typeid_t typeid_intern(type_t* t) {
  return t->_typeid ? t->_typeid : _typeid(t, true);
}
inline static typeid_t typeid_of(const type_t* t) {
  return t->_typeid ? t->_typeid : _typeid((type_t*)t, false);
}
inline static u32 typeid_len(typeid_t ti) {
  return *(u32*)(ti - 4);
}


ASSUME_NONNULL_END
