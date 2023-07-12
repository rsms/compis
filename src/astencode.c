// ast encoder
// SPDX-License-Identifier: Apache-2.0
#include "colib.h"
#include "compiler.h"
#include "astencode.h"
#include "hash.h"

#define KIND_CODE_NULL  ((u8)0x20)
#define KIND_CODE_START ((u8)0x21)
static_assert(((u32)KIND_CODE_START + (u32)(NODEKIND_COUNT - 1)) <= U8_MAX,
  "kind code overflows; need to reduce KIND_CODE_START");

#define MIN_ENCODED_NODE_SIZE \
  ALIGN2_X(strlen("XXXX 1 1 1\n"), 4)

#define ALLOC_ENCODED_NODE_SIZE \
  ALIGN2_X(strlen("XXXX 65535 4294967295 18446744073709551615\n"), 4)

/*
format

root = header
       symbol{symcount}
       node{nodecount-rootcount}
       node{rootcount}

header    = magic SP version SP symcount SP nodecount SP rootcount LF
magic     = "cAST"
version   = u32x
symcount  = u32x
nodecount = u32x
rootcount = u32x

symbol = <byte 0x01..0x09, 0x0B..0xFF>+ padding LF

node        = nodekind_id SP attr* padding LF
nodekind_id = (alnum | under | SP){4}
attr        = (uint | string | none | symref | noderef) SP
uint        = u8x | u16x | u32x | u64x
string      = '"' <byte 0x20..0xFF>+ '"' // compis-escaped string
symref      = "#" u32x
noderef     = "&" u32x
none        = "_"

padding  = SP{0,3}
alnum    = digit | <byte 'A'>
digit    = <byte '0'..'9'>
hexdigit = <byte '0'..'9' 'A'..'F'>
alpha    = <byte 'A'..'Z' 'a'..'z'>
under    = <byte '_'>
u8x      = hexdigit{1,2}
u16x     = hexdigit{1,4}
u32x     = hexdigit{1,8}
u64x     = hexdigit{1,16}
SP       = <byte 0x20>
LF       = <byte 0x0A>

*/


#define FILE_MAGIC_STR "cAST"
#define FILE_MAGIC_u32 CO_STRu32('cAST')


static const u32 nodekind_id_tab[NODEKIND_COUNT] = {
  [NODE_BAD]        = CO_STRu32('BAD '),
  [NODE_COMMENT]    = CO_STRu32('CMNT'),
  [NODE_UNIT]       = CO_STRu32('UNIT'),
  [STMT_TYPEDEF]    = CO_STRu32('TDEF'),
  [STMT_IMPORT]     = CO_STRu32('IMPO'),
  [EXPR_FUN]        = CO_STRu32('FUN '),
  [EXPR_BLOCK]      = CO_STRu32('BLK '),
  [EXPR_CALL]       = CO_STRu32('CALL'),
  [EXPR_TYPECONS]   = CO_STRu32('TCON'),
  [EXPR_ID]         = CO_STRu32('ID  '),
  [EXPR_FIELD]      = CO_STRu32('FIEL'),
  [EXPR_PARAM]      = CO_STRu32('PARM'),
  [EXPR_VAR]        = CO_STRu32('VAR '),
  [EXPR_LET]        = CO_STRu32('LET '),
  [EXPR_MEMBER]     = CO_STRu32('MEMB'),
  [EXPR_SUBSCRIPT]  = CO_STRu32('SUBS'),
  [EXPR_PREFIXOP]   = CO_STRu32('PREO'),
  [EXPR_POSTFIXOP]  = CO_STRu32('POSO'),
  [EXPR_BINOP]      = CO_STRu32('BINO'),
  [EXPR_ASSIGN]     = CO_STRu32('ASSI'),
  [EXPR_DEREF]      = CO_STRu32('DREF'),
  [EXPR_IF]         = CO_STRu32('IF  '),
  [EXPR_FOR]        = CO_STRu32('FOR '),
  [EXPR_RETURN]     = CO_STRu32('RET '),
  [EXPR_BOOLLIT]    = CO_STRu32('BLIT'),
  [EXPR_INTLIT]     = CO_STRu32('ILIT'),
  [EXPR_FLOATLIT]   = CO_STRu32('FLIT'),
  [EXPR_STRLIT]     = CO_STRu32('SLIT'),
  [EXPR_ARRAYLIT]   = CO_STRu32('ALIT'),
  [TYPE_VOID]       = CO_STRu32('void'),
  [TYPE_BOOL]       = CO_STRu32('bool'),
  [TYPE_I8]         = CO_STRu32('i8  '),
  [TYPE_I16]        = CO_STRu32('i16 '),
  [TYPE_I32]        = CO_STRu32('i32 '),
  [TYPE_I64]        = CO_STRu32('i64 '),
  [TYPE_INT]        = CO_STRu32('int '),
  [TYPE_U8]         = CO_STRu32('u8  '),
  [TYPE_U16]        = CO_STRu32('u16 '),
  [TYPE_U32]        = CO_STRu32('u32 '),
  [TYPE_U64]        = CO_STRu32('u64 '),
  [TYPE_UINT]       = CO_STRu32('uint'),
  [TYPE_F32]        = CO_STRu32('f32 '),
  [TYPE_F64]        = CO_STRu32('f64 '),
  [TYPE_ARRAY]      = CO_STRu32('arry'),
  [TYPE_FUN]        = CO_STRu32('fun '),
  [TYPE_PTR]        = CO_STRu32('ptr '),
  [TYPE_REF]        = CO_STRu32('ref '),
  [TYPE_MUTREF]     = CO_STRu32('mref'),
  [TYPE_SLICE]      = CO_STRu32('sli '),
  [TYPE_MUTSLICE]   = CO_STRu32('msli'),
  [TYPE_OPTIONAL]   = CO_STRu32('opt '),
  [TYPE_STRUCT]     = CO_STRu32('stct'),
  [TYPE_ALIAS]      = CO_STRu32('alia'),
  [TYPE_UNKNOWN]    = CO_STRu32('unkn'),
  [TYPE_UNRESOLVED] = CO_STRu32('unre'),
};


#ifdef DEBUG
UNUSED __attribute__((constructor)) static void check_nodekind_id_tab() {
  for (nodekind_t i = 0; i < (nodekind_t)countof(nodekind_id_tab); i++) {
    assertf(nodekind_id_tab[i] != 0, "missing nodekind_id_tab[%s]", nodekind_name(i));
    for (nodekind_t j = 0; j < (nodekind_t)countof(nodekind_id_tab); j++) {
      assertf(i == j || nodekind_id_tab[i] != nodekind_id_tab[j],
        "duplicate id \"%.4s\": [%s] & [%s]",
        (char*)&nodekind_id_tab[i], nodekind_name(i), nodekind_name(j));
    }
  }
}
// UNUSED __attribute__((constructor)) static void check_nodekind_id_tab() {
//   for (nodekind_t i = 0; i < (nodekind_t)countof(nodekind_id_tab); i++) {
//     assertf(*nodekind_id_tab[i], "missing nodekind_id_tab[%s]", nodekind_name(i));
//     for (nodekind_t j = 0; j < (nodekind_t)countof(nodekind_id_tab); j++) {
//       assertf(i == j || memcmp(nodekind_id_tab[i], nodekind_id_tab[j], 4) != 0,
//         "duplicate id \"%.4s\": [%s] & [%s]",
//         nodekind_id_tab[i], nodekind_name(i), nodekind_name(j));
//     }
//   }
// }
#endif


static usize read_u32(const u8* src, const u8* end, u32* result) {
  u32 n = 0;
  const u8* p = src;
  while (p < end && isdigit(*p))
    n = (10u * n) + (*p++ - '0');
  *result = n;
  return (usize)(uintptr)(p - src);
}


// ————————————————————————————————————————————————————————————————————————————————————
// decoder


typedef struct {
  u32 version;
  u32 nodecount;
} astdecode_t;


static const u8* skip_space(const u8* p, const u8* end) {
  while (p < end && isspace(*p))
    p++;
  return p;
}


err_t astdecode(
  astdecode_t* dec, memalloc_t ma, slice_t data
  // memalloc_t ast_ma, node_t** resultv, u32* resultc
) {
  if (!IS_ALIGN2((uintptr)data.p, 4))
    return ErrInvalid;

  const u8* inp = data.bytes;
  const u8* inend = inp + data.len;

  dlog("—— decoding ——");

  // check that header is reasonably long
  const usize kMinHeaderSize = strlen("XXXX 1 0\n");
  if (data.len < kMinHeaderSize) {
    dlog("header too small");
    return ErrInvalid;
  }

  // verify magic bytes
  if (*(u32*)inp != FILE_MAGIC_u32) {
    dlog("invalid magic bytes: %02x%02x%02x%02x", inp[0], inp[1], inp[2], inp[3]);
    return ErrInvalid;
  }
  inp += 4+1;

  // read version
  inp += read_u32(inp, inend, &dec->version);
  dlog("version: %u", dec->version);
  if (dec->version != 1) {
    dlog("unsupported version: %u", dec->version);
    return ErrNotSupported;
  }

  // read nodecount
  const u32 kMaxNodecount = 1024*1024; // TODO: make configurable
  inp = skip_space(inp, inend);
  inp += read_u32(inp, inend, &dec->nodecount);
  dlog("nodecount: %u", dec->nodecount);
  if (dec->nodecount > kMaxNodecount) {
    dlog("too many nodes: %u", dec->nodecount);
    return ErrOverflow;
  }

  // check if nodecount would need more bytes than available from input
  usize inp_remaining_len = (usize)(uintptr)(inend - inp);
  usize nodecount_size;
  if (check_mul_overflow(
    (usize)dec->nodecount, MIN_ENCODED_NODE_SIZE, &nodecount_size))
  {
    dlog("nodecount too large: %u", dec->nodecount);
    return ErrOverflow;
  }
  if (nodecount_size > inp_remaining_len) {
    dlog("nodecount too large: %u", dec->nodecount);
    return ErrOverflow;
  }

  // nmap {index => node_t*}
  node_t** nmap = mem_alloctv(ma, node_t*, (usize)dec->nodecount);
  if (!nmap)
    return ErrNoMem;

  // parse each node
  // TODO
  // - check bounds of nodecount vs index
  // - read kind
  // - allocate node_t and associate index in nmap
  // - read attributes

  mem_freetv(ma, nmap, (usize)dec->nodecount);

  return 0;
}


// ————————————————————————————————————————————————————————————————————————————————————
// encoder


static int ptr_cmp(const void** a, const void** b, void* ctx) {
  return *a == *b ? 0 : *a < *b ? -1 : 1;
}


// returns false if memory allocation failed
static bool reg_sym(astencode_t* a, sym_t sym) {
  sym_t* vp = array_sortedset_assign(
    sym_t, &a->symmap, a->ma, &sym, (array_sorted_cmp_t)ptr_cmp, NULL);
  if UNLIKELY(!vp)
    return false;
  if (*vp == NULL) {
    *vp = sym;
    a->symsize += ALIGN2(strlen(sym) + 1, 4);
  }
  return true;
}


static u32 sym_index(const astencode_t* a, sym_t sym) {
  u32 index;
  UNUSED sym_t* vp = array_sortedset_lookup(
    sym_t, &a->symmap, &sym, &index, (array_sorted_cmp_t)ptr_cmp, NULL);
  assertf(vp != NULL, "%s not in symmap", sym);
  return index;
}


static err_t reg_syms(astencode_t* a, const node_t* np) {
  bool ok = true;

  #define ADDSYM(sym)  ( ok &= reg_sym(a, assertnotnull(sym)) )
  #define ADDSYMZ(sym) ( (sym) ? ok &= reg_sym(a, (sym)) : ((void)0) )

  if (node_istype(np))
    ADDSYMZ(((const type_t*)np)->tid);

  switch ((enum nodekind)np->kind) {

  case EXPR_VAR:
  case EXPR_LET:
  case EXPR_FIELD: { const local_t* n = (local_t*)np;
    ADDSYM(n->name); break; }

  case EXPR_PARAM: { const local_t* n = (local_t*)np;
    ADDSYMZ(n->name); break; }

  case EXPR_ID: { const idexpr_t* n = (idexpr_t*)np;
    ADDSYM(n->name); break; }

  case EXPR_MEMBER: { const member_t* n = (member_t*)np;
    ADDSYM(n->name); break; }

  case EXPR_FUN: { const fun_t* n = (fun_t*)np;
    ADDSYMZ(n->name); break; }

  case TYPE_STRUCT: { const structtype_t* n = (structtype_t*)np;
    ADDSYMZ(n->name); break; }

  case TYPE_UNRESOLVED: { const unresolvedtype_t* n = (unresolvedtype_t*)np;
    ADDSYM(n->name); break; }

  case TYPE_ALIAS: { const aliastype_t* n = (aliastype_t*)np;
    ADDSYM(n->name); break; }

  // no symbols
  case NODE_BAD:
  case NODE_COMMENT:
  case NODE_UNIT:
  case STMT_IMPORT:  // TODO
  case STMT_TYPEDEF:
  case EXPR_ARRAYLIT:
  case EXPR_BLOCK:
  case EXPR_ASSIGN:
  case EXPR_BINOP:
  case EXPR_DEREF:
  case EXPR_POSTFIXOP:
  case EXPR_PREFIXOP:
  case EXPR_RETURN:
  case EXPR_CALL:
  case EXPR_IF:
  case EXPR_FOR:
  case EXPR_SUBSCRIPT:
  case EXPR_TYPECONS:
  case EXPR_BOOLLIT:
  case EXPR_INTLIT:
  case EXPR_FLOATLIT:
  case EXPR_STRLIT:
  case TYPE_VOID:
  case TYPE_BOOL:
  case TYPE_I8:
  case TYPE_I16:
  case TYPE_I32:
  case TYPE_I64:
  case TYPE_INT:
  case TYPE_U8:
  case TYPE_U16:
  case TYPE_U32:
  case TYPE_U64:
  case TYPE_UINT:
  case TYPE_F32:
  case TYPE_F64:
  case TYPE_ARRAY:
  case TYPE_FUN:
  case TYPE_PTR:
  case TYPE_REF:
  case TYPE_MUTREF:
  case TYPE_OPTIONAL:
  case TYPE_SLICE:
  case TYPE_MUTSLICE:
  case TYPE_UNKNOWN:
    break;

  }
  #undef ADDSYM
  return ErrNoMem * (err_t)!ok;
}


static u32 node_index(const astencode_t* a, const void* np) {
  assertf((uintptr)np >= 0x1000, "%p", np);
  uintptr* vp = (uintptr*)map_lookup_ptr(&a->nodemap, np);
  assertf(vp != NULL, "node %p %s not in nodemap",
    np, nodekind_name(((node_t*)np)->kind));
  // note: node indices are stored in nodemap with +1 larger value
  // to keep 0 reserved for "not in map"
  return (u32)(*vp - 1);
}


static void ptrarray_reverse(void** v, u32 len) {
  for (u32 i = 0, j = len - 1; i < j; i++, j--) {
    void* tmp = v[i];
    v[i] = v[j];
    v[j] = tmp;
  }
}


err_t astencode_add_ast(astencode_t* a, const node_t* n) {
  // nodes are ordered from least refs to most refs, i.e. children first, parents last.
  // e.g.
  //   n0 intlit   int 1
  //   n1 intlit   int 2
  //   n2 prefixop int &n1
  //   n3 binop    int &n0 &n2
  // This allows efficient decoding from top to bottom.
  // We accomplish this by using a logical queue of nodes to process,
  // filling it from children of each newfound node.
  // In practice we use the tail of the nodelist and finally reverse the added
  // range in place.
  err_t err;
  u32 nodelist_start = a->nodelist.len;

  if (!nodearray_push(&a->nodelist, a->ma, (void*)n))
    return ErrNoMem;

  u32 srcfileid = loc_srcfileid(n->loc);
  if (srcfileid > 0) {
    if (!u32array_sortedset_add(&a->srcfileids, a->ma, srcfileid))
      return ErrNoMem;
  }

  for (u32 i = nodelist_start; i < a->nodelist.len; i++) {
    const node_t* n = a->nodelist.v[i];
    uintptr* vp = (uintptr*)map_assign_ptr(&a->nodemap, a->ma, n);
    if (!vp)
      return ErrNoMem;
    if (*vp) {
      // remove duplicate
      //dlog("skip [%u] %s %p", i, nodekind_name(n->kind), n);
      nodearray_remove(&a->nodelist, i, 1);
      i--;
    } else {
      // explore new node
      //dlog("explore [%u] %s %p", i, nodekind_name(n->kind), n);
      *vp = UINTPTR_MAX; // ID placeholder
      if (( err = ast_childrenof(&a->nodelist, a->ma, n) ))
        return err;
    }
  }

  // check if the same root node has been provided twice
  if (nodelist_start == a->nodelist.len)
    return ErrExists;

  // reverse order of added nodes
  // TODO: figure out a way to avoid doing this
  ptrarray_reverse(
    (void**)&a->nodelist.v[nodelist_start], a->nodelist.len - nodelist_start);

  // register symbols in symset
  for (u32 i = nodelist_start; i < a->nodelist.len; i++)
    reg_syms(a, a->nodelist.v[i]);

  // assign node IDs
  for (u32 i = nodelist_start; i < a->nodelist.len;) {
    const node_t* n = a->nodelist.v[i];
    dlog("nodelist[%u] ← %s %p", i, nodekind_name(n->kind), n);
    uintptr* vp = (uintptr*)map_lookup_ptr(&a->nodemap, n);
    assertnotnull(vp);
    i++; // +1 to avoid 0 which would mess up map assignment
    *vp = (uintptr)i;
  }

  // add root node index to rootlist
  u32 index = a->nodelist.len - 1;
  //dlog("rootlist[%u] <= %u (nodelist[%u] = %s)",
  //  a->rootlist.len, index, index, nodekind_name(n->kind));
  if (!u32array_push(&a->rootlist, a->ma, index))
    return ErrNoMem;

  return err;
}


static usize append_aligned_linebreak_unchecked(u8* dst) {
  usize n = 0;
  uintptr addr = (uintptr)dst;
  if (!IS_ALIGN2(addr + 1, 4)) {
    usize aligned_len = ALIGN2(addr + 1, 4) - 1;
    n = aligned_len - addr;
    memset(dst, ' ', n);
  }
  dst[n] = '\n';
  return n + 1;
}


static err_t append_aligned_linebreak(buf_t* outbuf) {
  if (!buf_reserve(outbuf, 4))
    return ErrNoMem;
  outbuf->len += append_aligned_linebreak_unchecked(outbuf->bytes + outbuf->len);
  return 0;
}


static void encode_header(astencode_t* a, buf_t* outbuf) {
  // write header
  u32 version = 1; // FIXME
  char* p = outbuf->chars + outbuf->len;
  memcpy(p, FILE_MAGIC_STR, 4); p += 4; *p++ = ' ';
  p += fmt_u64_base10(p, 10, version); *p++ = ' ';
  p += fmt_u64_base10(p, 10, a->symmap.len); *p++ = ' ';
  p += fmt_u64_base10(p, 10, a->nodelist.len); *p++ = ' ';
  p += fmt_u64_base10(p, 10, a->rootlist.len);
  *p++ = '\n'; // p += append_aligned_linebreak_unchecked((u8*)p);
  outbuf->len += (usize)(uintptr)(p - (outbuf->chars + outbuf->len));
}


static void encode_syms(astencode_t* a, buf_t* outbuf) {
  // space has been pre-allocated in outbuf by our caller
  u8* p = outbuf->bytes + outbuf->len;
  for (u32 i = 0; i < a->symmap.len; i++) {
    sym_t sym = a->symmap.v[i];
    // dlog("sym[%u] = '%s'", i, sym);
    usize symlen = strlen(sym);
    assert(p + symlen + 1 <= outbuf->bytes + outbuf->cap);
    memcpy(p, sym, symlen); p += symlen;
    *p++ = '\n'; // p += append_aligned_linebreak_unchecked(p);
  }
  outbuf->len += (usize)(uintptr)(p - (outbuf->bytes + outbuf->len));
}


// Reserve enough space for kMaxPlainVals u64 values and separators.
// Note: MUST make a separate call to buf_reserve when writing strings.
#define kMaxPlainVals 8ul
#define kMinAlloc     ALIGN2_X(kMaxPlainVals * (20 + 1), 4)


static bool encode_string(
  astencode_t* a, buf_t* outbuf, char** pp, const char* src, usize srclen)
{
  // "flush" p offset to allow buf_ functions to accurately calculate available space
  outbuf->len += (usize)(uintptr)(*pp - (outbuf->chars + outbuf->len));

  bool ok = buf_push(outbuf, '"');
  ok &= buf_appendrepr(outbuf, src, srclen);
  ok &= buf_push(outbuf, '"');

  // update pp
  *pp = outbuf->chars + outbuf->len;
  return ok;
}


static bool encode_nodearray(astencode_t* a, buf_t* outbuf, char** pp, nodearray_t na) {
  outbuf->len += (usize)(uintptr)(*pp - (outbuf->chars + outbuf->len));

  usize nbyte = 9ul + ((usize)na.len * 10); // "*LEN &id &id ..."
  if (!buf_reserve(outbuf, nbyte))
    return false;

  char* p = outbuf->chars + outbuf->len;
  *p++ = '*';
  p += fmt_u64_base16(p, 8, (u64)na.len);
  for (u32 i = 0; i < na.len; i++) {
    *p++ = ' ';
    *p++ = '&';
    p += fmt_u64_base16(p, 8, (u64)node_index(a, na.v[i]));
  }

  // bool ok = buf_push(outbuf, '*');
  // ok &= buf_print_u32(outbuf, na.len, 16);
  // for (u32 i = 0; i < na.len && ok; i++) {
  //   ok &= buf_push(outbuf, ' ');
  //   ok &= buf_push(outbuf, '&');
  //   ok &= buf_print_u32(outbuf, node_index(a, na.v[i]), 16);
  // }

  *pp = p;
  return true;
}


static err_t encode_node_attrs(astencode_t* a, buf_t* outbuf, const node_t* np) {
  if (!buf_reserve(outbuf, kMinAlloc))
    return ErrNoMem;
  char* p = outbuf->chars + outbuf->len;
  bool ok = true;

  #define NONE()         ( *p++ = '_', ((void)0) )
  #define SEP()          ( *p++ = ' ', ((void)0) )
  #define U32X(v)        ( p += fmt_u64_base16(p, 8, (u64)(v)), ((void)0) )
  #define U64X(v)        ( p += fmt_u64_base16(p, 16, (v)), ((void)0) )
  #define SYMREF(sym)    ( *p++ = '#', U32X( sym_index(a, (sym)) ) )
  #define SYMREFZ(sym)   ( (sym) ? SYMREF(sym) : NONE() )
  #define NODEREF(n)     ( *p++ = '&', U32X( node_index(a, (n)) ) )
  #define NODEREFZ(n)    ( (n) ? NODEREF(n) : NONE() )
  #define STRINGN(s,len) ( ok &= encode_string(a, outbuf, &p, (s), (len)) )
  #define STRING(cstr)   ( STRINGN((cstr), strlen(cstr)) )
  #define STRINGZ(cstr)  ( (cstr) ? STRING(cstr) : NONE() )
  #define NODEARRAY(pa)  ( ok &= encode_nodearray(a, outbuf, &p, *(pa)) )

  // type of expression
  if (node_isexpr(np))
    SEP(), NODEREFZ( ((const expr_t*)np)->type );

  switch ((enum nodekind)np->kind) {

  case STMT_TYPEDEF: { const typedef_t* n = (typedef_t*)np;
    SEP(), NODEREF(n->type);
    break; }

  case EXPR_ID: { const idexpr_t* n = (idexpr_t*)np;
    SEP(), SYMREF(n->name);
    break; }

  case EXPR_VAR:
  case EXPR_LET:
  case EXPR_PARAM:
  case EXPR_FIELD: { const local_t* n = (local_t*)np;
    SEP(), SYMREF(n->name);
    SEP(), U64X(n->nameloc);
    SEP(), U64X(n->offset);
    SEP(), NODEREFZ(n->init);
    break; }

  case TYPE_STRUCT: { const structtype_t* n = (structtype_t*)np;
    SEP(), SYMREFZ(n->name);
    SEP(), STRINGZ(n->mangledname);
    SEP(), NODEARRAY(&n->fields);
    // TODO: list of nodes

    // TODO: nsparent is currently not included with ast_childrenof and so
    // might not be indexed
    // SEP(), SYMREFZ(n->nsparent);
    break; }

  case TYPE_FUN: /*{ const funtype_t* n = (funtype_t*)np;
    ADD_ARRAY_OF_NODES(&n->params);
    ADD_NODE(n->result); break; }*/

  // TODO:
  case NODE_UNIT:
  case EXPR_ARRAYLIT:
  case EXPR_BLOCK:
  case EXPR_ASSIGN:
  case EXPR_BINOP:
  case EXPR_DEREF:
  case EXPR_POSTFIXOP:
  case EXPR_PREFIXOP:
  case EXPR_RETURN:
  case EXPR_CALL:
  case EXPR_IF:
  case EXPR_FOR:
  case EXPR_FUN:
  case EXPR_MEMBER:
  case EXPR_SUBSCRIPT:
  case EXPR_TYPECONS:
  case TYPE_ALIAS:
  case TYPE_ARRAY:
  case TYPE_PTR:
  case TYPE_REF:
  case TYPE_MUTREF:
  case TYPE_OPTIONAL:
  case TYPE_SLICE:
  case TYPE_MUTSLICE:
  case TYPE_UNRESOLVED:
    dlog("TODO %s", nodekind_name(np->kind));
    break;

  // no attributes
  case NODE_BAD:
  case NODE_COMMENT:
  case STMT_IMPORT:
  case EXPR_BOOLLIT:
  case EXPR_INTLIT:
  case EXPR_FLOATLIT:
  case EXPR_STRLIT:
  case TYPE_VOID:
  case TYPE_BOOL:
  case TYPE_I8:
  case TYPE_I16:
  case TYPE_I32:
  case TYPE_I64:
  case TYPE_INT:
  case TYPE_U8:
  case TYPE_U16:
  case TYPE_U32:
  case TYPE_U64:
  case TYPE_UINT:
  case TYPE_F32:
  case TYPE_F64:
  case TYPE_UNKNOWN:
    break;

  } // switch

  #undef NONE
  #undef SEP
  #undef U32X
  #undef U64X
  #undef SYMREF
  #undef SYMREFZ
  #undef NODEREF
  #undef NODEREFZ

  if (!ok)
    return false;
  outbuf->len += (usize)(uintptr)(p - (outbuf->chars + outbuf->len));
  return 0;
}


static err_t encode_node(astencode_t* a, buf_t* outbuf, const node_t* n) {
  // allocate space
  if UNLIKELY(!buf_reserve(outbuf, ALLOC_ENCODED_NODE_SIZE))
    return ErrNoMem;

  assertf(IS_ALIGN2(outbuf->len, 4), "%zu", outbuf->len);

  // buf_printf(outbuf, "%-4u", node_index(a, n)); // XXX

  // write 4-byte node kind ID
  *(u32*)&outbuf->bytes[outbuf->len] = nodekind_id_tab[n->kind];
  outbuf->len += 4;

  // // NODEFLAGS_NOENCODE: flags to exclude from AST encoding
  // #define NODEFLAGS_NOENCODE  (NF_CHECKED | NF_UNKNOWN)
  // buf_push(outbuf, ' '); buf_print_u32(outbuf, n->flags & ~NODEFLAGS_NOENCODE, 16);

  // common attributes of node_t
  buf_push(outbuf, ' '); buf_print_u32(outbuf, n->flags, 16);
  buf_push(outbuf, ' '); buf_print_u32(outbuf, n->nuse, 16);

  // TODO: remap srcfile
  buf_push(outbuf, ' '); buf_print_u64(outbuf, n->loc, 16);
  // loc_t loc = loc_make_unchecked(0,
  //   loc_line(n->loc), loc_col(n->loc), loc_width(n->loc));
  // buf_push(outbuf, ' '); buf_print_u64(outbuf, loc, 16);

  // attributes
  err_t err = encode_node_attrs(a, outbuf, n);
  if (err)
    return err;

  return append_aligned_linebreak(outbuf);
}


static usize calc_min_allocsize(astencode_t* a) {
  // approximate space needed, starting with the header
  usize nbyte = 4 + 1   // magic SP
              + 10 + 1  // version SP
              + 10 + 1  // symcount SP
              + 10 + 1  // nodecount SP
              + 10 + 1; // rootcount LF

  // add space needed to encode symbols
  nbyte += a->symsize;

  // align to 4-byte boundary for nodes
  nbyte = ALIGN2(nbyte, 4);

  // approximate space needed to encode nodes
  usize nbyte_nodes;
  if (check_mul_overflow((usize)a->nodelist.len, MIN_ENCODED_NODE_SIZE, &nbyte_nodes))
    goto overflow;

  // add space needed to encode nodes to nbyte
  if (check_add_overflow(nbyte_nodes, nbyte, &nbyte))
    goto overflow;

  assert(nbyte < USIZE_MAX);
  return ALIGN2(nbyte, 4);
overflow:
  return USIZE_MAX;
}


err_t astencode_encode(astencode_t* a, buf_t* outbuf) {
  // allocate space in outbuf
  usize nbyte = calc_min_allocsize(a);
  if (nbyte == USIZE_MAX)
    return ErrOverflow;
  if UNLIKELY(!buf_reserve(outbuf, nbyte))
    return ErrNoMem;

  // write header
  encode_header(a, outbuf);

  // write symbols
  encode_syms(a, outbuf);

  // replace last '\n' with a padded '\n'
  outbuf->len += append_aligned_linebreak_unchecked(
    outbuf->bytes + (outbuf->len - 1)) - 1;

  // write nodes
  err_t err = 0;
  for (u32 i = 0; i < a->nodelist.len && !err; i++)
    err = encode_node(a, outbuf, a->nodelist.v[i]);
  // for (u32 i = a->nodelist.len; i > 0 && !err; )
  //   err = encode_node(a, outbuf, a->nodelist.v[--i]);

  // write root-node IDs
  nbyte = (usize)a->rootlist.len * 9; // "FFFFFFFF\n"
  if UNLIKELY(!buf_reserve(outbuf, nbyte))
    return ErrNoMem;
  for (u32 i = 0; i < a->rootlist.len && !err; i++) {
    u32 node_i = a->rootlist.v[i];
    outbuf->len += fmt_u64_base16(outbuf->chars + outbuf->len, 8, (u64)node_i);
    outbuf->chars[outbuf->len++] = '\n';
  }

  return err;
}


err_t astencode_init(astencode_t* a, memalloc_t ma) {
  memset(a, 0, sizeof(*a));
  a->ma = ma;
  err_t err = map_init(&a->nodemap, ma, 256) ? 0 : ErrNoMem;
  return err;
}


void astencode_dispose(astencode_t* a) {
  nodearray_dispose(&a->nodelist, a->ma);
  ptrarray_dispose(&a->symmap, a->ma);
  u32array_dispose(&a->rootlist, a->ma);
  u32array_dispose(&a->srcfileids, a->ma);
  map_dispose(&a->nodemap, a->ma);
}
