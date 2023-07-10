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

#define MIN_ENCODED_NODE_SIZE 8ul // "XXXX\n\n\n\n"

/*
format

root = header node*

header    = magic SP version SP nodecount LF
magic     = "cAST"
version   = u32
nodecount = u32

node        = nodekind_id
nodekind_id = (alnum | under | SP){4}

alnum = digit | <byte 'A'>
digit = <byte '0'..'9'>
alpha = <byte 'A'..'Z' 'a'..'z'>
under = <byte '_'>
u32   = digit (digit | SP)*
SP    = <byte 0x20>
LF    = <byte 0x0A>
*/

#if CO_LITTLE_ENDIAN
  #define HTONu32(x) ( \
    (((x) >> 24) & 0x000000FF) | \
    (((x) >> 8)  & 0x0000FF00) | \
    (((x) << 8)  & 0x00FF0000) | \
    (((x) << 24) & 0xFF000000) \
  )
  #define STRu32(code) HTONu32((u32)(code))
#else
  #define STRu32(code) ((u32)(code))
#endif


#define FILE_MAGIC_STR "cAST"
#define FILE_MAGIC_u32 STRu32('cAST')


static const u32 nodekind_id_tab[NODEKIND_COUNT] = {
  [NODE_BAD]        = STRu32('BAD '),
  [NODE_COMMENT]    = STRu32('CMNT'),
  [NODE_UNIT]       = STRu32('UNIT'),
  [STMT_TYPEDEF]    = STRu32('TDEF'),
  [STMT_IMPORT]     = STRu32('IMPO'),
  [EXPR_FUN]        = STRu32('FUN '),
  [EXPR_BLOCK]      = STRu32('BLOK'),
  [EXPR_CALL]       = STRu32('CALL'),
  [EXPR_TYPECONS]   = STRu32('TCON'),
  [EXPR_ID]         = STRu32('ID  '),
  [EXPR_FIELD]      = STRu32('FIEL'),
  [EXPR_PARAM]      = STRu32('PARM'),
  [EXPR_VAR]        = STRu32('VAR '),
  [EXPR_LET]        = STRu32('LET '),
  [EXPR_MEMBER]     = STRu32('MEMB'),
  [EXPR_SUBSCRIPT]  = STRu32('SUSC'),
  [EXPR_PREFIXOP]   = STRu32('PROP'),
  [EXPR_POSTFIXOP]  = STRu32('POOP'),
  [EXPR_BINOP]      = STRu32('BIOP'),
  [EXPR_ASSIGN]     = STRu32('ASSI'),
  [EXPR_DEREF]      = STRu32('DERE'),
  [EXPR_IF]         = STRu32('IF  '),
  [EXPR_FOR]        = STRu32('FOR '),
  [EXPR_RETURN]     = STRu32('RETU'),
  [EXPR_BOOLLIT]    = STRu32('BOOL'),
  [EXPR_INTLIT]     = STRu32('INTL'),
  [EXPR_FLOATLIT]   = STRu32('FLOA'),
  [EXPR_STRLIT]     = STRu32('STRL'),
  [EXPR_ARRAYLIT]   = STRu32('ARRA'),
  [TYPE_VOID]       = STRu32('void'),
  [TYPE_BOOL]       = STRu32('bool'),
  [TYPE_I8]         = STRu32('i8  '),
  [TYPE_I16]        = STRu32('i16 '),
  [TYPE_I32]        = STRu32('i32 '),
  [TYPE_I64]        = STRu32('i64 '),
  [TYPE_INT]        = STRu32('int '),
  [TYPE_U8]         = STRu32('u8  '),
  [TYPE_U16]        = STRu32('u16 '),
  [TYPE_U32]        = STRu32('u32 '),
  [TYPE_U64]        = STRu32('u64 '),
  [TYPE_UINT]       = STRu32('uint'),
  [TYPE_F32]        = STRu32('f32 '),
  [TYPE_F64]        = STRu32('f64 '),
  [TYPE_ARRAY]      = STRu32('arry'),
  [TYPE_FUN]        = STRu32('fun '),
  [TYPE_PTR]        = STRu32('ptr '),
  [TYPE_REF]        = STRu32('ref '),
  [TYPE_MUTREF]     = STRu32('mref'),
  [TYPE_SLICE]      = STRu32('sli '),
  [TYPE_MUTSLICE]   = STRu32('msli'),
  [TYPE_OPTIONAL]   = STRu32('opt '),
  [TYPE_STRUCT]     = STRu32('stct'),
  [TYPE_ALIAS]      = STRu32('alia'),
  [TYPE_UNKNOWN]    = STRu32('unkn'),
  [TYPE_UNRESOLVED] = STRu32('unre'),
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


static err_t add_ast(astencode_t* a, const node_t* n) {
  // check if nodemap (and return early if so) or register np as visited
  uintptr* vp = (uintptr*)map_assign_ptr(&a->nodemap, a->ma, n);
  if (!vp || *vp != 0)
    return vp == NULL ? ErrNoMem : 0;
  *vp = (uintptr)U32_MAX; // placeholder

  // visit children of node
  a->children.len = 0;
  err_t err = ast_childrenof(&a->children, a->ma, n);
  for (u32 i = 0; i < a->children.len && !err; i++)
    err = add_ast(a, a->children.v[i]);

  // add node to nodelist
  if (!err) {
    dlog("nodelist[%u] <= %s %p", a->nodelist.len, nodekind_name(n->kind), n);
    if (!ptrarray_push(&a->nodelist, a->ma, (void*)n))
      err = ErrNoMem;
  }

  // store nodelist index (+1 to avoid )
  *vp = (uintptr)a->nodelist.len;

  return err;
}


err_t astencode_add_ast(astencode_t* a, const node_t* n) {
  // nodes are ordered from least refs to most refs, i.e. children first, parents last.
  // e.g.
  //   n0 intlit   int 1
  //   n1 intlit   int 2
  //   n2 prefixop int &n1
  //   n3 binop    int &n0 &n2
  // This allows efficient decoding from top to bottom.
  // We do this with depth-first traversal of the AST.

  u32 nodelist_len = a->nodelist.len;

  err_t err = add_ast(a, n);
  if (err)
    return err;

  // duplicate; same node provided to astencode_add_ast twice
  if (a->nodelist.len - nodelist_len == 0)
    return ErrExists;

  // add to rootlist
  u32 index = a->nodelist.len - 1;
  dlog("rootlist[%u] <= nodelist[%u] = %s",
    a->rootlist.len, index, nodekind_name(n->kind));
  if (!u32array_push(&a->rootlist, a->ma, index))
    return ErrNoMem;

  return err;
}


static err_t append_aligned_linebreak(buf_t* outbuf) {
  if (!IS_ALIGN2(outbuf->len + 1, 4)) {
    usize aligned_len = ALIGN2(outbuf->len + 1, 4) - 1;
    usize padding_len = aligned_len - outbuf->len;
    if UNLIKELY(buf_avail(outbuf) <= padding_len) { // <= since we add '\n' later
      if (!buf_reserve(outbuf, padding_len))
        return ErrNoMem;
    }
    memset(&outbuf->bytes[outbuf->len], ' ', padding_len);
    outbuf->len = aligned_len;
  } else if UNLIKELY(buf_avail(outbuf) == 0) {
    if (!buf_reserve(outbuf, 4))
      return ErrNoMem;
  }
  outbuf->bytes[outbuf->len++] = '\n';
  return 0;
}


static void encode_header(astencode_t* a, buf_t* outbuf) {
  // write header
  u32 version = 1; // FIXME
  char* p = outbuf->chars + outbuf->len;
  memcpy(p, FILE_MAGIC_STR, 4); p += 4; *p++ = ' ';
  p += fmt_u64_base10(p, 10, version); *p++ = ' ';
  p += fmt_u64_base10(p, 10, a->nodelist.len); *p++ = ' ';
  p += fmt_u64_base10(p, 10, a->rootlist.len);
  outbuf->len += (usize)(uintptr)(p - (outbuf->chars + outbuf->len));
  safecheckx(append_aligned_linebreak(outbuf) == 0); // can't fail
}


static err_t encode_node_attrs(astencode_t* a, buf_t* outbuf, const node_t* np) {
  switch ((enum nodekind)np->kind) {
  // no children
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

  case STMT_TYPEDEF: { const typedef_t* n = (typedef_t*)np;
    buf_reserve(outbuf, 16);
    u32 node_id = 123; // FIXME
    outbuf->chars[outbuf->len++] = ' ';
    outbuf->len += fmt_u64_base10(outbuf->chars + outbuf->len, 10, node_id);
    // ADD_NODE(&n->type);
    break; }

  // todo
  case NODE_UNIT:
  case EXPR_ARRAYLIT:
  case EXPR_BLOCK:
  case EXPR_ASSIGN:
  case EXPR_BINOP:
  case EXPR_DEREF:
  case EXPR_POSTFIXOP:
  case EXPR_PREFIXOP:
  case EXPR_ID:
  case EXPR_RETURN:
  case EXPR_VAR:
  case EXPR_LET:
  case EXPR_PARAM:
  case EXPR_FIELD:
  case EXPR_CALL:
  case EXPR_IF:
  case EXPR_FOR:
  case EXPR_FUN:
  case EXPR_MEMBER:
  case EXPR_SUBSCRIPT:
  case EXPR_TYPECONS:
  case TYPE_ALIAS:
  case TYPE_ARRAY:
  case TYPE_FUN:
  case TYPE_PTR:
  case TYPE_REF:
  case TYPE_MUTREF:
  case TYPE_OPTIONAL:
  case TYPE_SLICE:
  case TYPE_MUTSLICE:
  case TYPE_STRUCT:
  case TYPE_UNRESOLVED:
    dlog("TODO %s", nodekind_name(np->kind));
    break;
  }
  return 0;
}


static err_t encode_node(astencode_t* a, buf_t* outbuf, const node_t* n) {
  // allocate space
  if UNLIKELY(!buf_reserve(outbuf, MIN_ENCODED_NODE_SIZE + 4))
    return ErrNoMem;

  assertf(IS_ALIGN2(outbuf->len, 4), "%zu", outbuf->len);

  // write 4-byte node kind ID
  *(u32*)&outbuf->bytes[outbuf->len] = nodekind_id_tab[n->kind];
  outbuf->len += 4;

  // attributes
  err_t err = encode_node_attrs(a, outbuf, n);
  if (err)
    return err;

  return append_aligned_linebreak(outbuf);
}


err_t astencode_encode(astencode_t* a, buf_t* outbuf) {
  // approximate space needed, starting with the header
  usize nbyte = 4 + 1   // magic SP
              + 10 + 1  // version SP
              + 10 + 1  // nodecount SP
              + 10 + 1; // rootcount LF
  nbyte = ALIGN2(nbyte, 4);

  // approximate space needed to encode nodes
  usize nbyte_nodes;
  if (check_mul_overflow((usize)a->nodelist.len, MIN_ENCODED_NODE_SIZE, &nbyte_nodes))
    return ErrOverflow;

  // add space needed to encode nodes to nbyte
  if (check_add_overflow(nbyte_nodes, nbyte, &nbyte))
    return ErrOverflow;

  // allocate space in outbuf
  if UNLIKELY(!buf_reserve(outbuf, ALIGN2(nbyte, 4)))
    return ErrNoMem;

  // write header
  encode_header(a, outbuf);

  // rootlist cursor
  // Note: a neat property of root node detection is that node indices in nodelist
  // are in the same order as entries in the rootlist, which means that all we have
  // to do to check if a node is in the rootlist is compare it to rootlistp, and if
  // it's a match we increment rootlistp.
  u32 rootlist_i = 0;
  u32 next_root_id = a->rootlist.len > 0 ? a->rootlist.v[0] : a->nodelist.len;

  // write non-root nodes
  err_t err = 0;
  for (u32 i = 0; i < a->nodelist.len && !err; i++) {
    if (i == next_root_id) {
      // defer encoding of root node for later
      rootlist_i++;
      if (rootlist_i < a->rootlist.len)
        next_root_id = a->rootlist.v[rootlist_i];
    } else {
      err = encode_node(a, outbuf, a->nodelist.v[i]);
    }
  }

  // write root nodes
  for (u32 i = 0; i < a->rootlist.len && !err; i++) {
    u32 node_i = a->rootlist.v[i];
    err = encode_node(a, outbuf, a->nodelist.v[node_i]);
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
  ptrarray_dispose(&a->children, a->ma);
  ptrarray_dispose(&a->nodelist, a->ma);
  u32array_dispose(&a->rootlist, a->ma);
  map_dispose(&a->nodemap, a->ma);
}
