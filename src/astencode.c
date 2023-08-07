// SPDX-License-Identifier: Apache-2.0
/*

AST encoding format:

  root = header
         pkg
         srcfile{srccount}
         pkg{importcount}
         symbol{symcount}
         node{nodecount}
         nodeid{rootcount}

  header      = magic SP
                version SP
                srccount SP
                importcount SP
                symcount SP
                nodecount SP
                rootcount LF
  magic       = "cAST"
  version     = u32x
  srccount    = u32x
  symcount    = u32x
  importcount = u32x
  nodecount   = u32x
  rootcount   = u32x

  pkg     = pkgroot ":" pkgpath (":" sha256x)? LF
  pkgroot = filepath
  pkgpath = filepath
  srcfile = filepath LF

  symbol = <byte 0x01..0x09, 0x0B..0xFF>+ LF

  node      = nodekind (attr (SP attr)*)? LF
  nodekind  = (alnum | under | SP){4}
  attr      = (uint | string | none | symref | noderef | nodearray)
  uint      = u8x | u16x | u32x | u64x
  string    = '"' <byte 0x20..0xFF>* '"' // compis-escaped
  symref    = "#" symbolid
  noderef   = "&" nodeid
  nodearray = "*" len (SP nodeid){len}
  none      = "_"
  nodeid    = u32x
  symbolid  = u32x

  sha256x  = hexdigit{64}
  filepath = <byte 0x20..0x39, 0x3B...0xFF>+  // note: 0x40 = ":"
  len      = u32x
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
#include "colib.h"
#include "astencode.h"
#include "ast_field.h"
#include "compiler.h"
#include "path.h"
#include "hash.h"
#include "sha256.h"


#define FILE_MAGIC "cAST"
#define AST_ENC_VERSION 1

// DEBUG_LOG_ENCODE_STATS: define to dlog some encoder stats
//#define DEBUG_LOG_ENCODE_STATS

#if defined(DEBUG_LOG_ENCODE_STATS) && !defined(DEBUG)
  #undef DEBUG_LOG_ENCODE_STATS
#endif


// Conversion of f64 <-> u64
// Currently assumes f64 is represented as IEEE 754 binary64
inline static u64 f64_to_u64(f64 v) { return *(u64*)&v; }
inline static f64 u64_to_f64(u64 v) { return *(f64*)&v; }


//———————————————————————————————————————————————————————————————————————————————————————
// encoder

typedef struct astencoder_ {
  compiler_t*       c;
  memalloc_t        ma;
  array_type(mem_t) tmpallocs;  // temporary allocations
  nodearray_t       nodelist;   // all nodes
  u32array_t        rootlist;   // indices in nodelist of root nodes
  u32array_t        srcfileids; // unique loc_t srcfile IDs
  map_t             nodemap;    // maps {node_t* => uintptr nodelist index}
  ptrarray_t        symmap;     // maps {sym_t => u32 index} (sorted set)
  usize             symsize;    // total length of all symbol characters
  const pkg_t*      pkg;        //
  bool              oom;        // true if memory allocation failed (internal state)
} astencoder_t;


// buf_setlenp sets buf->len to the offset of p relative to the start of buf
inline static void buf_setlenp(buf_t* buf, void* p) {
  assertf(p >= buf->p && p <= buf->p + buf->cap, "%p", p);
  buf->len = (usize)(uintptr)((u8*)p - buf->bytes);
}


#define BUF_RESERVE(nbyte, retval...) ({ \
  usize nbyte__ = (nbyte); \
  if UNLIKELY( \
    (buf_avail(outbuf) < nbyte__ && !buf_reserve(outbuf, nbyte__)) || a->oom) \
  { \
    if (!a->oom) \
      dlog("OOM buf_reserve(%zu B) in %s", nbyte__, __FUNCTION__); \
    a->oom = true; \
    return retval; \
  } \
})


static int ptr_cmp(const void** a, const void** b, void* ctx) {
  return *a == *b ? 0 : *a < *b ? -1 : 1;
}


static u32 encoded_sym_index(const astencoder_t* a, sym_t sym) {
  u32 index;
  UNUSED sym_t* vp = array_sortedset_lookup(
    sym_t, &a->symmap, &sym, &index, (array_sorted_cmp_t)ptr_cmp, NULL);
  assertf(vp != NULL, "%s not in symmap", sym);
  return index;
}


static u32 encoded_node_index(const astencoder_t* a, const void* np) {
  uintptr* vp = (uintptr*)map_lookup_ptr(&a->nodemap, np);
  assertf(vp != NULL, "node %p %s not in nodemap",
    np, nodekind_name(((node_t*)np)->kind));
  assertf(*vp != 0, "nodemap entry for %p %s is 0",
    np, nodekind_name(((node_t*)np)->kind));
  // note: node indices are stored in nodemap with +1 larger value
  // to keep 0 reserved for "not in map"
  return (u32)(*vp - 1);
}


static void encode_str(astencoder_t* a, buf_t* outbuf, const char* str, usize len) {
  usize bufavail, w;

  if (len > U32_MAX) {
    // TODO: signal "overflow" rather than "OOM"
    a->oom = true;
    return;
  }

  outbuf->chars[outbuf->len++] = '"';

try_string_repr:
  bufavail = buf_avail(outbuf);
  w = string_repr(outbuf->chars + outbuf->len, bufavail, str, len);

  if UNLIKELY(w + 2 >= bufavail) { // +2 for final '"' and node-terminating LF
    // not enough room at p; str needs to be escaped
    if (!buf_grow(outbuf, (w + 2) - buf_avail(outbuf))) {
      a->oom = true;
      return;
    }
    goto try_string_repr;
  }

  outbuf->len += w;
  outbuf->chars[outbuf->len++] = '"';
}


static void encode_field_nodearray(
  astencoder_t* a, buf_t* outbuf, const void* fp, ast_field_t f)
{
  // We have space for max possible node IDs, so no bounds checks needed here
  const nodearray_t* na = fp;
  char* p = outbuf->chars + outbuf->len;

  *p++ = '*';
  p += sfmtu64(p, na->len, 16);

  for (u32 i = 0; i < na->len; i++) {
    *p++ = ' ';
    p += sfmtu64(p, encoded_node_index(a, na->v[i]), 16);
  }

  buf_setlenp(outbuf, p);
}


/*static void encode_field_nodelist(
  astencoder_t* a, buf_t* outbuf, const void* fp, ast_field_t f)
{
  // same encoding as nodearray
  char* p = outbuf->chars + outbuf->len;

  u32 len = 0;
  for (const void* ent = *(void**)fp; ent; ent = *(void**)(ent + f.listoffs))
    len++;

  *p++ = '*';
  p += sfmtu64(p, len, 16);

  for (const void* ent = *(void**)fp; ent; ent = *(void**)(ent + f.listoffs)) {
    *p++ = ' ';
    p += sfmtu64(p, encoded_node_index(a, ent), 16);
  }

  buf_setlenp(outbuf, p);
}*/


static void encode_field_str(
  astencoder_t* a, buf_t* outbuf, const void* fp, ast_field_t f)
{
  const char* str = *(const char**)fp;
  usize len = strlen(str);
  encode_str(a, outbuf, str, len);
}


static int u32_cmp(const u32* a, const u32* b, void* ctx) {
  return *a == *b ? 0 : *a < *b ? -1 : 1;
}


static loc_t enc_remap_loc(astencoder_t* a, loc_t loc) {
  // search a->srcfileids, which is a sorted u32array
  u32 srcfileid = loc_srcfileid(loc);
  if (srcfileid == 0) // unknown srcfile
    return loc;

  u32 index;
  u32* vp = array_sortedset_lookup(
    u32, (const array_t*)&a->srcfileids, &srcfileid, &index,
    (array_sorted_cmp_t)u32_cmp, NULL);
  srcfileid = vp ? (index + 1) : 0; // +1: srcfileid is 1-based

  // dlog("re-map srcfileid %u => %u", loc_srcfileid(loc), srcfileid);
  return loc_with_srcfileid(loc, srcfileid);
}


// field_encsize calculates the space needed to encode a field
static usize field_encsize(astencoder_t* a, const void* fp, ast_field_t f) {
  usize z = 1; // leading SP
again:
  switch ((enum ast_fieldtype)f.type) {
    case AST_FIELD_NODEZ:
    case AST_FIELD_SYMZ:
    case AST_FIELD_STRZ:
      if (*(void**)fp == NULL) return z + 1; // "_"
      f.type--; // e.g. AST_FIELD_NODEZ -> AST_FIELD_NODE
      goto again;

    case AST_FIELD_U8:   return z + ndigits16(*(u8*)fp);
    case AST_FIELD_U16:  return z + ndigits16(*(u16*)fp);
    case AST_FIELD_U32:  return z + ndigits16(*(u32*)fp);
    case AST_FIELD_U64:  return z + ndigits16(*(u64*)fp);
    case AST_FIELD_F64:  return z + 16; // TODO FIXME
    case AST_FIELD_LOC:  return z + ndigits16(*(loc_t*)fp);
    case AST_FIELD_SYM:
      return z + 1 + ndigits16(encoded_sym_index(a, *(sym_t*)fp)); // "#" u32x
    case AST_FIELD_NODE:
      return z + 1 + ndigits16(encoded_node_index(a, *(node_t**)fp)); // "&" u32x

    case AST_FIELD_STR: { // '"' strdata '"'
      // optimistic guess: the string does not need escaping
      usize len = strlen(*(const char**)fp);
      //return z + 3ul + ndigits16(len) + len; // e.g. "B hello world"
      return z + 2 + len; // e.g. "hello world"
    }

    case AST_FIELD_NODEARRAY: { // "*" len (SP u32x){len}
      // conservative guess: node indices are max, e.g. *2 FFFFFFFF FFFFFFFF
      // This trades some memory slack for a simpler implementation
      const nodearray_t* na = fp;
      return z + 1ul + ndigits16(na->len) + (na->len * (1 + 9));
    }

    // case AST_FIELD_NODELIST: { // "*" len (SP u32x){len}  (same as nodearray)
    //   u32 len = 0;
    //   for (const void* ent = *(void**)fp; ent; ent = *(void**)(ent + f.listoffs))
    //     len++;
    //   return z + 1ul + ndigits16(len) + (len * (1 + 9));
    // }

    case AST_FIELD_UNDEF:
      break;
  }
  UNREACHABLE;
  return z;
}


static void encode_field(astencoder_t* a, buf_t* outbuf, const void* fp, ast_field_t f) {
  //dlog("  %-12s %-9s  +%u", f.name, ast_fieldtype_str(f.type), f.offs);

  // set fp to point to the field's data
  fp += f.offs;

  // calculate and allocate space needed to encode the field
  usize nbyte = field_encsize(a, fp, f);
  nbyte++; // make sure we always have space for LF to terminate the node
  if UNLIKELY(buf_avail(outbuf) < nbyte) {
    //dlog("    grow outbuf by %zu B", nbyte - buf_avail(outbuf));
    if (!buf_grow(outbuf, nbyte - buf_avail(outbuf))) {
      a->oom = true;
      return;
    }
  }

  u64 u64val;
  outbuf->chars[outbuf->len++] = ' ';

  switch ((enum ast_fieldtype)f.type) {
  case AST_FIELD_U8:        u64val = *(u8*)fp; goto enc_u64x;
  case AST_FIELD_U16:       u64val = *(u16*)fp; goto enc_u64x;
  case AST_FIELD_U32:       u64val = *(u32*)fp; goto enc_u64x;
  case AST_FIELD_U64:       u64val = *(u64*)fp; goto enc_u64x;
  case AST_FIELD_F64:       u64val = f64_to_u64(*(f64*)fp); goto enc_u64x;
  case AST_FIELD_LOC:       u64val = enc_remap_loc(a, *(loc_t*)fp); goto enc_u64x;
  case AST_FIELD_SYM:       goto enc_sym;
  case AST_FIELD_SYMZ:      if (*(void**)fp) goto enc_sym; goto enc_none;
  case AST_FIELD_NODE:      goto enc_node;
  case AST_FIELD_NODEZ:     if (*(void**)fp) goto enc_node; goto enc_none;
  case AST_FIELD_STR:       goto enc_str;
  case AST_FIELD_STRZ:      if (*(void**)fp) goto enc_str; goto enc_none;
  case AST_FIELD_NODEARRAY: MUSTTAIL return encode_field_nodearray(a, outbuf, fp, f);
  //case AST_FIELD_NODELIST:  MUSTTAIL return encode_field_nodelist(a, outbuf, fp, f);
  case AST_FIELD_UNDEF:     break;
  }
  UNREACHABLE;

enc_u64x:
  outbuf->len += sfmtu64(outbuf->chars + outbuf->len, u64val, 16);
  return;

enc_none:
  outbuf->chars[outbuf->len++] = '_';
  return;

enc_node:
  outbuf->chars[outbuf->len++] = '&';
  u64val = encoded_node_index(a, *(const node_t**)fp);
  goto enc_u64x;

enc_sym:
  outbuf->chars[outbuf->len++] = '#';
  u64val = encoded_sym_index(a, *(sym_t*)fp);
  goto enc_u64x;

enc_str: {}
  MUSTTAIL return encode_field_str(a, outbuf, fp, f);
}


// NODE_BASE_ENCSIZE: bytes needed to encode a node's basic attributes
#define NODE_BASE_ENCSIZE  strlen("XXXX FFFF FFFFFFFF FFFFFFFFFFFFFFFF\n")


static void encode_node(astencoder_t* a, buf_t* outbuf, const node_t* n) {
  assertf(n->kind < NODEKIND_COUNT, "%s %u", nodekind_name(n->kind), n->kind);

  // Reserve enough space for base node attributes (kind, flags etc) and fields.
  // Note that astencoder_encode has preallocated memory already so in most cases
  // this is just a check on "buf_avail(outbuf) >= NODE_BASE_ENCSIZE" that passes.
  // Only in cases where a previous node wrote long string data does this cause
  // outbuf to grow.
  BUF_RESERVE(NODE_BASE_ENCSIZE);

  // get pointer to outbuf memory
  u8* p = outbuf->bytes + outbuf->len;

  // kind
  memcpy(p, &g_ast_kindtagtab[n->kind], 4); p += 4;

  // get field table for kind
  const ast_field_t* fieldtab = g_ast_fieldtab[n->kind];

  // check for universal type
  if (fieldtab == g_fieldsof_type_t) {
    // universal type is encoded solely by kind
    buf_setlenp(outbuf, p);
  } else {
    // it's a standard node, not a universal one

    // exclude NF_MARK* from flags
    nodeflag_t flags = n->flags & ~(NF_MARK1 | NF_MARK2);

    // base attributes of node_t
    *p++ = '\t'; p += fmt_u64_base16((char*)p, 4, (u64)flags);
    *p++ = '\t'; p += fmt_u64_base16((char*)p, 8, (u64)n->nuse);
    *p++ = '\t'; p += fmt_u64_base16((char*)p, 16, (u64)enc_remap_loc(a, n->loc));

    // update outbuf->len
    buf_setlenp(outbuf, p);

    // encode fields
    for (u8 i = 0; i < g_ast_fieldlentab[n->kind]; i++) {
      encode_field(a, outbuf, n, fieldtab[i]);
      if (a->oom)
        return;
    }
  }

  // add terminating LF (encode_field ensures that there's space for it in outbuf)
  assert(buf_avail(outbuf) > 0);
  outbuf->chars[outbuf->len++] = '\n';
}


static void encode_header(astencoder_t* a, buf_t* outbuf) {
  char* p = outbuf->chars + outbuf->len;
  memcpy(p, FILE_MAGIC, 4); p += 4; *p++ = ' ';
  p += fmt_u64_base16(p, 8, AST_ENC_VERSION); *p++ = ' ';
  p += fmt_u64_base16(p, 8, a->srcfileids.len); *p++ = ' ';
  p += fmt_u64_base16(p, 8, a->pkg->imports.len); *p++ = ' ';
  p += fmt_u64_base16(p, 8, a->symmap.len); *p++ = ' ';
  p += fmt_u64_base16(p, 8, a->nodelist.len); *p++ = ' ';
  p += fmt_u64_base16(p, 8, a->rootlist.len);
  *p++ = '\n';
  buf_setlenp(outbuf, p);
}


static void encode_filepath(astencoder_t* a, buf_t* outbuf, const char* str, usize len) {
  // filepath = <byte 0x20..0x39, 0x3B...0xFF>+  // note: 0x40 = ":"
  #ifdef CO_SAFE
  for (usize i = 0; i < len; i++) {
    if UNLIKELY(str[i] < 0x20 || str[i] == ':') {
      dlog("invalid char 0x%02x in filepath \"%.*s\"",
        str[i], (int)MIN((usize)I32_MAX, len), str);
      a->oom = true; // TODO: signal more accurate error
      return;
    }
  }
  #endif

  a->oom |= !buf_append(outbuf, str, len);
}


static void encode_pkg(astencoder_t* a, buf_t* outbuf, const pkg_t* pkg) {
  encode_filepath(a, outbuf, pkg->root.p, pkg->root.len);
  outbuf->chars[outbuf->len++] = ':';
  encode_filepath(a, outbuf, pkg->path.p, pkg->path.len);
  if (!sha256_iszero(&pkg->api_sha256)) {
    outbuf->chars[outbuf->len++] = ':';
    buf_appendhex(outbuf, &pkg->api_sha256, sizeof(pkg->api_sha256));
  }
  outbuf->chars[outbuf->len++] = '\n';
}


static void encode_srcfiles(astencoder_t* a, buf_t* outbuf) {
  for (u32 i = 0; i < a->srcfileids.len; i++) {
    const srcfile_t* srcfile = locmap_srcfile(&a->c->locmap, a->srcfileids.v[i]);
    encode_filepath(a, outbuf, srcfile->name.p, srcfile->name.len);
    outbuf->chars[outbuf->len++] = '\n';

    // verify that sources are from just one package
    assertnotnull(srcfile->pkg);
    assertf(a->pkg == srcfile->pkg,
      "srcfiles from mixed packages %s (%p) and %s (%p)",
      a->pkg->path.p, a->pkg, srcfile->pkg->path.p, srcfile->pkg);
  }
}


static void encode_imports(astencoder_t* a, buf_t* outbuf) {
  for (u32 i = 0; i < a->pkg->imports.len; i++)
    encode_pkg(a, outbuf, a->pkg->imports.v[i]);
}


static void encode_syms(astencoder_t* a, buf_t* outbuf) {
  BUF_RESERVE(a->symsize);
  u8* p = outbuf->bytes + outbuf->len;
  for (u32 i = 0; i < a->symmap.len; i++) {
    sym_t sym = a->symmap.v[i];
    // dlog("sym[%u] = '%s' %p", i, sym, sym);
    usize symlen = strlen(sym);
    assert(p + symlen + 1 <= outbuf->bytes + outbuf->cap);
    memcpy(p, sym, symlen); p += symlen;
    *p++ = '\n'; // p += append_aligned_linebreak_unchecked(p);
  }
  buf_setlenp(outbuf, p);
}


static usize enc_preallocsize(astencoder_t* a) {
  // start with space needed for the header
  // Rather than using ndigits for an accurate estimation, we're assuming
  // "the worst"; we will most certainly need to use more space than what
  // we allocate since certain node fields require buffer expansion.
  usize nbyte = 4 + 1  // magic SP
              + ndigits16(AST_ENC_VERSION) + 1  // version SP
              + 8 + 1  // srccount SP
              + 8 + 1  // importcount SP
              + 8 + 1  // symcount SP
              + 8 + 1  // nodecount SP
              + 8 + 1  // rootcount LF
  ;

  // add space needed to encode pkg (pkgroot ":" pkgpath (":" sha256x)? LF)
  a->oom |= check_add_overflow(nbyte, a->pkg->root.len + 1, &nbyte);
  a->oom |= check_add_overflow(nbyte, a->pkg->path.len + 1, &nbyte);
  a->oom |= check_add_overflow(nbyte, 64ul + 1, &nbyte);

  // add space needed to encode srcfiles
  for (u32 i = 0; i < a->srcfileids.len; i++) {
    const srcfile_t* srcfile = locmap_srcfile(&a->c->locmap, a->srcfileids.v[i]);
    assertnotnull(srcfile);
    a->oom |= check_add_overflow(nbyte, srcfile->name.len + 1, &nbyte);
  }

  // add space needed to encode imports
  for (u32 i = 0; i < a->pkg->imports.len; i++) {
    const pkg_t* dep = a->pkg->imports.v[i];
    a->oom |= check_add_overflow(nbyte, dep->root.len + 1, &nbyte);
    a->oom |= check_add_overflow(nbyte, dep->path.len + 1, &nbyte);
    a->oom |= check_add_overflow(nbyte, 64ul + 1, &nbyte);
  }

  // add space needed to encode symbols
  a->oom |= check_add_overflow(nbyte, a->symsize, &nbyte);

  // approximate space needed to encode nodes in nodelist
  // nbyte += NODE_BASE_ENCSIZE * nodelist.len
  usize nbyte_nodes;
  a->oom |= check_mul_overflow(NODE_BASE_ENCSIZE, (usize)a->nodelist.len, &nbyte_nodes);
  a->oom |= check_add_overflow(nbyte, nbyte_nodes, &nbyte);

  // calculate space needed to encode root node IDs ("XXXXXXXX\n")
  // nbyte_rootids = 9 * rootlist.len
  usize nbyte_rootids;
  usize maxdigits = ndigits16(a->nodelist.len - 1) + 1; // +1 for '\n'
  a->oom |= check_mul_overflow(maxdigits, (usize)a->rootlist.len, &nbyte_rootids);

  // add space needed to encode root node IDs
  a->oom |= check_add_overflow(nbyte, nbyte_rootids, &nbyte);

  // align to pointer size, since that is what the allocator will do anyways
  nbyte = ALIGN2(nbyte, sizeof(void*));

  assert(nbyte < USIZE_MAX); // catch false negatives of overflow signalling
  return nbyte;
}


err_t astencoder_encode(astencoder_t* a, buf_t* outbuf) {
  assertf(a->pkg != NULL, "astencoder_begin not called before astencoder_encode");

  if (a->oom)
    return ErrNoMem;

  // allocate space in outbuf
  usize nbyte = enc_preallocsize(a);
  if (a->oom)
    return ErrOverflow;
  #ifdef DEBUG_LOG_ENCODE_STATS
    dlog("%s: allocating %zu B in outbuf (%u syms, %u nodes, %u roots)",
      __FUNCTION__, nbyte, a->symmap.len, a->nodelist.len, a->rootlist.len);
    usize debug_outbuf_initlen = outbuf->len;
  #endif
  BUF_RESERVE(nbyte, ErrNoMem);

  encode_header(a, outbuf);
  encode_pkg(a, outbuf, a->pkg);
  encode_srcfiles(a, outbuf);
  encode_imports(a, outbuf);
  encode_syms(a, outbuf);

  // write nodes
  for (u32 i = 0; i < a->nodelist.len; i++)
    encode_node(a, outbuf, a->nodelist.v[i]);
  if (a->oom)
    return ErrNoMem;

  // write root node IDs (make sure we have space in outbuf)
  BUF_RESERVE((usize)a->rootlist.len * 9, ErrNoMem);
  for (u32 i = 0; i < a->rootlist.len; i++) {
    u32 node_i = a->rootlist.v[i];
    outbuf->len += fmt_u64_base16(outbuf->chars + outbuf->len, 8, (u64)node_i);
    outbuf->chars[outbuf->len++] = '\n';
  }

  #ifdef DEBUG_LOG_ENCODE_STATS
    usize nbyte_used = outbuf->len - debug_outbuf_initlen;
    dlog("%s: used %zu B of outbuf (%zu%s, %zu total cap)", __FUNCTION__,
      nbyte_used,
      ( nbyte_used < nbyte ? (nbyte - nbyte_used) :
                             (nbyte_used - nbyte) ),
      ( nbyte_used < nbyte ? " less than preallocated" :
        nbyte_used > nbyte ? " more than preallocated" :
                             "" ),
      outbuf->cap
    );
  #endif

  return 0;
}


// ———————————————— adding AST to be encoded ————————————————


static void reg_sym(astencoder_t* a, sym_t sym) {
  sym_t* vp = array_sortedset_assign(
    sym_t, &a->symmap, a->ma, &sym, (array_sorted_cmp_t)ptr_cmp, NULL);
  if UNLIKELY(!vp) {
    a->oom = true;
  } else if (*vp == NULL) {
    *vp = sym;
    a->symsize += strlen(sym) + 1;
  }
}


static void reg_syms(astencoder_t* a, const node_t* n) {
  if (a->oom)
    return;

  #define ADDSYM(sym)  ( reg_sym(a, assertnotnull(sym)) )
  #define ADDSYMZ(sym) ( (sym) ? reg_sym(a, (sym)) : ((void)0) )

  // if (node_istype(n))
  //   ADDSYMZ(((const type_t*)n)->_typeid);

  switch ((enum nodekind)n->kind) {

  case EXPR_VAR:
  case EXPR_LET:
  case EXPR_FIELD:      return ADDSYM(((local_t*)n)->name);

  case NODE_TPLPARAM:   return ADDSYM(((templateparam_t*)n)->name);
  case EXPR_PARAM:      return ADDSYMZ(((local_t*)n)->name);
  case EXPR_ID:         return ADDSYM(((idexpr_t*)n)->name);
  case EXPR_NS:         return ADDSYM(((nsexpr_t*)n)->name);
  case EXPR_MEMBER:     return ADDSYM(((member_t*)n)->name);
  case EXPR_FUN:        return ADDSYMZ(((fun_t*)n)->name);
  case TYPE_STRUCT:     return ADDSYMZ(((structtype_t*)n)->name);
  case TYPE_UNRESOLVED: return ADDSYM(((unresolvedtype_t*)n)->name);
  case TYPE_ALIAS:      return ADDSYM(((aliastype_t*)n)->name);

  // no symbols
  case NODE_BAD:
  case NODE_COMMENT:
  case NODE_UNIT:
  case NODE_IMPORTID:
  case STMT_IMPORT:  // TODO
  case STMT_TYPEDEF:
  case EXPR_ARRAYLIT:
  case EXPR_ASSIGN:
  case EXPR_BINOP:
  case EXPR_BLOCK:
  case EXPR_BOOLLIT:
  case EXPR_CALL:
  case EXPR_DEREF:
  case EXPR_FLOATLIT:
  case EXPR_FOR:
  case EXPR_IF:
  case EXPR_INTLIT:
  case EXPR_POSTFIXOP:
  case EXPR_PREFIXOP:
  case EXPR_RETURN:
  case EXPR_STRLIT:
  case EXPR_SUBSCRIPT:
  case EXPR_TYPECONS:
  case TYPE_ARRAY:
  case TYPE_BOOL:
  case TYPE_F32:
  case TYPE_F64:
  case TYPE_FUN:
  case TYPE_I16:
  case TYPE_I32:
  case TYPE_I64:
  case TYPE_I8:
  case TYPE_INT:
  case TYPE_MUTREF:
  case TYPE_MUTSLICE:
  case TYPE_NS:
  case TYPE_OPTIONAL:
  case TYPE_PLACEHOLDER:
  case TYPE_PTR:
  case TYPE_REF:
  case TYPE_SLICE:
  case TYPE_TEMPLATE:
  case TYPE_U16:
  case TYPE_U32:
  case TYPE_U64:
  case TYPE_U8:
  case TYPE_UINT:
  case TYPE_UNKNOWN:
  case TYPE_VOID:
    break;
  }
  #undef ADDSYM
  #undef ADDSYMZ
}


static void* nullable enc_tmpalloc(astencoder_t* a, usize size) {
  mem_t m = mem_alloc(a->ma, size);
  if (m.p) {
    if UNLIKELY(!array_push(mem_t, (array_t*)&a->tmpallocs, a->ma, m)) {
      mem_freex(a->ma, m);
      m.p = NULL;
      a->oom = true;
    }
  } else {
    a->oom = true;
  }
  return m.p;
}


static void* nullable clone_node_shallow(astencoder_t* a, const node_t* n) {
  usize nodesize = g_ast_sizetab[n->kind];
  node_t* n2 = enc_tmpalloc(a, nodesize);
  if LIKELY(n2)
    memcpy(n2, n, nodesize);
  return n2;
}


static const node_t* pub_api_filter_unit(astencoder_t* a, const unit_t* unit1) {
  // first, count public children
  u32 pubchildcount = 0;
  for (u32 i = 0; i < unit1->children.len; i++)
    pubchildcount += (u32)!!(unit1->children.v[i]->flags & NF_VIS_PUB);

  // no filtering needed if all children are public
  if (pubchildcount == unit1->children.len)
    return (node_t*)unit1;

  // make a copy of unit1 which we can modify
  unit_t* unit2 = clone_node_shallow(a, (node_t*)unit1);

  // no public children?
  if (pubchildcount == 0) {
    unit2->children.len = 0;
    return (node_t*)unit2;
  }

  // allocate memory for new children array
  node_t** newchildrenv = enc_tmpalloc(a, (usize)pubchildcount * sizeof(void*));
  if (!newchildrenv) // OOM
    return (node_t*)unit1;
  unit2->children.len = pubchildcount;
  unit2->children.cap = pubchildcount;
  unit2->children.v = newchildrenv;

  // populate unit2->children.v
  for (u32 i1 = 0, i2 = 0; i2 < pubchildcount; i1++) {
    const node_t* cn = unit1->children.v[i1];
    if (cn->flags & NF_VIS_PUB)
      unit2->children.v[i2++] = (node_t*)cn;
  }

  return (node_t*)unit2;
}


static const node_t* pub_api_filter_node(astencoder_t* a, const node_t* n) {
  switch (n->kind) {

    // function definitions become declarations in PUB_API mode
    case EXPR_FUN: {
      const fun_t* fn = (fun_t*)n;
      if (!fn->body)
        break;
      fun_t* n2 = clone_node_shallow(a, n);
      n2->body = NULL;
      return (node_t*)n2;
    }

    case NODE_UNIT:
      return pub_api_filter_unit(a, (const unit_t*)n);

  }
  return n;
}


static void add_ast_visitor(astencoder_t* a, u32 flags, const node_t* n) {
  // assign n to nodemap, returning early if it's already in the map
  uintptr* vp = (uintptr*)map_assign_ptr(&a->nodemap, a->ma, n);
  if UNLIKELY(!vp) {
    dlog("%s: map_assign_ptr OOM", __FUNCTION__);
    a->oom = true;
    return;
  }
  if (*vp != 0) {
    // dlog("skip already-visited %s %p", nodekind_name(n->kind), n);
    return;
  }

  // assign placeholder ID (can't be 0 since we use that in the check above)
  *vp = UINTPTR_MAX;

  if (flags & ASTENCODER_PUB_API)
    n = pub_api_filter_node(a, n);

  // visit expression's type
  if (node_isexpr(n) && ((expr_t*)n)->type) {
    const expr_t* expr = (expr_t*)n;
    // dlog("visit type %s %p", nodekind_name(expr->type->kind), expr->type);
    add_ast_visitor(a, flags, (node_t*)expr->type);
  }

  // visit each child
  #if 1
  ast_childit_t childit = ast_childit_const(n);
  for (const node_t* cn; (cn = ast_childit_const_next(&childit));) {
    // dlog("visit child %s %p", nodekind_name(cn->kind), cn);
    add_ast_visitor(a, flags, cn);
  }
  #else
  astiter_t childit = astiter_of_children(n);
  for (const node_t* cn; (cn = astiter_next(&childit));) {
    // dlog("visit child %s %p", nodekind_name(cn->kind), cn);
    add_ast_visitor(a, flags, cn);
  }
  astiter_dispose(&childit);
  #endif

  // append n to nodelist
  // dlog("add nodelist[%u] <= %s %p", a->nodelist.len, nodekind_name(n->kind), n);
  if UNLIKELY(!nodearray_push(&a->nodelist, a->ma, (void*)n) && !a->oom) {
    dlog("%s: nodearray_push OOM", __FUNCTION__);
    a->oom = true;
  }

  // assign its nodelist index to nodemap
  *vp = a->nodelist.len; // +1 since 0 is initial value, checked for above
}


err_t astencoder_add_srcfileid(astencoder_t* a, u32 srcfileid) {
  if (srcfileid > 0) {
    assertf(locmap_srcfile(&a->c->locmap, srcfileid), "%u not found", srcfileid);
    if (!u32array_sortedset_add(&a->srcfileids, a->ma, srcfileid))
      return ErrNoMem;
  }
  return 0;
}


err_t astencoder_add_srcfile(astencoder_t* a, const srcfile_t* srcfile) {
  // u32 srcfileid = locmap_lookup_srcfileid(&a->c->locmap, srcfile);
  // if (srcfileid == 0)
  //   return ErrNotFound;

  u32 srcfileid = locmap_intern_srcfileid(&a->c->locmap, srcfile, a->c->ma);
  if UNLIKELY(srcfileid == 0) {
    dlog("locmap_srcfileid OOM");
    return ErrNoMem;
  }

  if (!u32array_sortedset_add(&a->srcfileids, a->ma, srcfileid))
    return ErrNoMem;
  return 0;
}


err_t astencoder_add_ast(astencoder_t* a, const node_t* n, u32 flags) {
  // nodes are ordered from least refs to most refs (children first, parents last) e.g.
  //   (<int> + (<int> intlit 1) (<int> intlit 2))
  // is encoded as:
  //   n0 inttype
  //   n1 intlit  &n0 1
  //   n2 intlit  &n0 2
  //   n3 binop   &n0 "+" &n1 &n2
  // This allows efficient decoding from top to bottom.
  u32 nodelist_start = a->nodelist.len;

  // register source file
  astencoder_add_srcfileid(a, loc_srcfileid(n->loc));

  // add n and all its children to nodelist using a depth-first traversal of n
  //
  //   fun visit(n)
  //     if isexpr(n)
  //       visit(n.type)
  //     for child in children_iterator(n)
  //       visit(child)
  //     nodelist.push(n)
  //
  add_ast_visitor(a, flags, n);
  if (a->oom)
    return ErrNoMem;

  // register symbols of added nodes
  for (u32 i = nodelist_start; i < a->nodelist.len; i++)
    reg_syms(a, a->nodelist.v[i]);
  if (a->oom)
    return ErrNoMem;

  u32 node_id;
  if (nodelist_start == a->nodelist.len) {
    // no nodes were added
    node_id = ptrarray_rindexof((const ptrarray_t*)&a->nodelist, n);
    assertf(node_id < U32_MAX, "%p not found in nodelist", n);
  } else {
    // n was added last, so its ID is simply nodelist.len - 1
    node_id = a->nodelist.len - 1;
  }

  // it's illegal to call astencoder_add_ast twice with the same node
  assertf(u32array_rindexof(&a->rootlist, node_id) == U32_MAX,
    "%s %p added twice", nodekind_name(n->kind), n);

  // add ID of n to rootlist
  // dlog("rootlist[%u] <= %u (nodelist[%u] = %s %p)",
  //   a->rootlist.len, node_id, node_id, nodekind_name(n->kind), n);
  if (!u32array_push(&a->rootlist, a->ma, node_id)) {
    a->oom = true;
    return ErrNoMem;
  }

  return 0;
}


astencoder_t* nullable astencoder_create(compiler_t* c) {
  astencoder_t* a = mem_alloct(c->ma, astencoder_t);
  if (!a)
    return NULL;
  a->ma = c->ma;
  a->c = c;
  if (!map_init(&a->nodemap, c->ma, 256)) {
    mem_freet(c->ma, a);
    return NULL;
  }
  return a;
}


void astencoder_free(astencoder_t* a) {
  for (u32 i = 0; i < a->tmpallocs.len; i++)
    mem_freex(a->ma, a->tmpallocs.v[i]);
  array_dispose(mem_t, (array_t*)&a->tmpallocs, a->ma);

  nodearray_dispose(&a->nodelist, a->ma);
  ptrarray_dispose(&a->symmap, a->ma);
  u32array_dispose(&a->rootlist, a->ma);
  u32array_dispose(&a->srcfileids, a->ma);
  map_dispose(&a->nodemap, a->ma);

  mem_freet(a->ma, a);
}


void astencoder_begin(astencoder_t* a, const pkg_t* pkg) {
  a->nodelist.len = 0;
  a->symmap.len = 0;
  a->rootlist.len = 0;
  a->srcfileids.len = 0;
  a->pkg = pkg;
  map_clear(&a->nodemap);
}


//———————————————————————————————————————————————————————————————————————————————————————
// decoder

typedef struct astdecoder_ {
  u32         version;
  u32         symcount;    // length of symtab
  u32         nodecount;   // length of nodetab
  u32         rootcount;   // number of root nodes at the tail of nodetab
  u32         srccount;    // length of srctab
  u32         importcount; //
  sym_t*      symtab;      // ID => sym_t
  node_t**    nodetab;     // ID => node_t*
  u32*        srctab;      // ID => srcfileid (document local ID => global ID)
  nodearray_t tmpnodearray;
  memalloc_t  ma;
  memalloc_t  ast_ma;
  compiler_t* c;
  const char* srcname;
  const u8*   pstart;
  const u8*   pend;
  const u8*   pcurr;
  err_t       err;
} astdecoder_t;

#define DEC_PARAMS  astdecoder_t* d, const u8* const pend, const u8* p
#define DEC_ARGS    d, pend, p

#define DEC_DATA_AVAIL  ((usize)(uintptr)(pend - p))


const char* astdecoder_srcname(const astdecoder_t* d) {
  return d->srcname;
}


memalloc_t astdecoder_ast_ma(const astdecoder_t* d) {
  return d->ast_ma;
}


static void decoder_error_loc(DEC_PARAMS, u32* linenop, u32* colp) {
  u32 lineno = 1;
  const u8* linestart = d->pstart;
  for (const u8* p1 = d->pstart; p1 < p; p1++) {
    if (*p1 == '\n') {
      linestart = p1 + 1;
      lineno++;
    }
  }
  usize col = (usize)(uintptr)(p - linestart);
  *colp = col >= U32_MAX ? 0 : (u32)(col+1);
  *linenop = lineno;
}


static const u8* dec_error(DEC_PARAMS, err_t err) {
  if (d->err != 0)
    return pend;
  d->err = err;

  u32 line, col;
  decoder_error_loc(DEC_ARGS, &line, &col);
  //usize offs = (usize)(uintptr)(p - d->pstart);
  u8 b = p < pend ? *p : 0;
  elog("AST decoding error: %s:%u:%u: 0x%02x '%c'",
    d->srcname, line, col, b, isprint(b) ? b : ' ');

  return pend;
}

// u8* DEC_ERROR(DEC_PARAMS, err_t err)
// u8* DEC_ERROR(DEC_PARAMS, err_t err, const char* fmt, ...)
#define DEC_ERROR(err, ...) __VARG_DISP(_DEC_ERROR,err,##__VA_ARGS__)
#define _DEC_ERROR1(err) ( \
  dlog("[%s] decoding error", __FUNCTION__), \
  dec_error(DEC_ARGS, err) \
)
#define _DEC_ERRORF(err, debug_fmt, debug_args...) ( \
  dlog("[%s] decoding error: " debug_fmt, __FUNCTION__, ##debug_args), \
  dec_error(DEC_ARGS, err) \
)
#define _DEC_ERROR2 _DEC_ERRORF
#define _DEC_ERROR3 _DEC_ERRORF
#define _DEC_ERROR4 _DEC_ERRORF
#define _DEC_ERROR5 _DEC_ERRORF
#define _DEC_ERROR6 _DEC_ERRORF
#define _DEC_ERROR7 _DEC_ERRORF
#define _DEC_ERROR8 _DEC_ERRORF
#define _DEC_ERROR9 _DEC_ERRORF


static const u8* dec_uintx(DEC_PARAMS, u64* result, u64 limit) {
  usize srclen = (usize)(uintptr)(pend - p);
  err_t err = co_intscan(&p, srclen, /*base*/16, limit, result);
  if UNLIKELY(err) {
    return DEC_ERROR(err,
      "%s", err == ErrOverflow ? "value too large" : "invalid integer");
  }
  if UNLIKELY(p == pend || (*p != ' ' && *p != '\t' && *p != '\n'))
    return DEC_ERROR(err, "%s", p == pend ? "end of input" : "bad int terminator");
  return p;
}

static const u8* dec_u64x(DEC_PARAMS, u64* result) {
  return dec_uintx(DEC_ARGS, result, 0xffffffffffffffffllu);
}

static const u8* dec_u32x(DEC_PARAMS, u32* result) {
  u64 tmp;
  p = dec_uintx(DEC_ARGS, &tmp, 0xffffffff);
  *result = (u32)tmp;
  return p;
}

static const u8* dec_u16x(DEC_PARAMS, u16* result) {
  u64 tmp;
  p = dec_uintx(DEC_ARGS, &tmp, 0xffff);
  *result = (u16)tmp;
  return p;
}

static const u8* dec_u8x(DEC_PARAMS, u8* result) {
  u64 tmp;
  p = dec_uintx(DEC_ARGS, &tmp, 0xff);
  *result = (u8)tmp;
  return p;
}

static const u8* dec_loc(DEC_PARAMS, loc_t* result) {
  static_assert(sizeof(u64) == sizeof(loc_t), "");

  loc_t loc;
  p = dec_u64x(DEC_ARGS, (u64*)&loc);
  if (d->err)
    return p;

  // re-map document-local ID to global ID
  u32 doc_srcfileid = loc_srcfileid(loc);
  if (doc_srcfileid > 0) {
    if UNLIKELY(doc_srcfileid > d->srccount)
      return DEC_ERROR(ErrNotFound, "invalid srcfile ID %u", doc_srcfileid);
    // dlog("re-map srcfileid %u => %u", doc_srcfileid, d->srctab[doc_srcfileid - 1]);
    *result = loc_with_srcfileid(loc, d->srctab[doc_srcfileid - 1]);
  }

  return p;
}

static const u8* dec_f64x(DEC_PARAMS, f64* result) {
  u64 tmp;
  p = dec_uintx(DEC_ARGS, &tmp, 0xffffffffffffffffllu);
  *result = u64_to_f64(tmp);
  return p;
}


static const u8* dec_byte(DEC_PARAMS, u8 byte) {
  if LIKELY(p < pend && *p == byte)
    return p + 1;
  return DEC_ERROR(ErrInvalid, "expected byte 0x%02x", byte);
}


static const u8* dec_whitespace(DEC_PARAMS) {
  if UNLIKELY( ! (p < pend && (*p == ' ' || *p == '\t')) )
    return DEC_ERROR(ErrInvalid, "expected whitespace");
  while (p < pend && (*p == ' ' || *p == '\t'))
    p++;
  return p;
}


static const u8* dec_ref(DEC_PARAMS,
  void** dst, bool allow_null, u8 prefix_char, void** tabv, u32 tabc)
{
  // prefix_char "0"
  if UNLIKELY(DEC_DATA_AVAIL < 2 || *p != prefix_char) {
    if LIKELY(*p == '_' && allow_null) {
      *dst = NULL;
      return p + 1;
    }
    #if DEBUG
      if (*p == '_')
        return DEC_ERROR(ErrInvalid, "NULL where null is not allowed");
      return DEC_ERROR(ErrInvalid, "expected '%cN'", (char)prefix_char);
    #else
      return DEC_ERROR(ErrInvalid);
    #endif
  }

  p += 1; // prefix_char

  u32 id;
  p = dec_u32x(DEC_ARGS, &id);
  if UNLIKELY(d->err != 0 || id > tabc) {
    if UNLIKELY(id > tabc)
      return DEC_ERROR(ErrInvalid, "invalid ID 0x%x", id);
    return p;
  }

  assertf(tabv[id] != NULL, "tabv[%u]", id);
  *dst = tabv[id];
  return p;
}


static const u8* dec_symref(DEC_PARAMS, sym_t* dst, bool allow_null) {
  return dec_ref(DEC_ARGS,
    (void**)dst, allow_null, '#', (void**)d->symtab, d->symcount);
}


static const u8* dec_noderef(DEC_PARAMS, node_t** dst, bool allow_null) {
  return dec_ref(DEC_ARGS,
    (void**)dst, allow_null, '&', (void**)d->nodetab, d->nodecount);
}


static const u8* dec_str(DEC_PARAMS, u8** dstp, bool allow_null) {
  // string = '"' <byte 0x20..0xFF>* '"' // compis-escaped
  if UNLIKELY(DEC_DATA_AVAIL < 2 || *p != '"') { // min size is 2 ("")
    if LIKELY(*p == '_' && allow_null) {
      *dstp = NULL;
      return p + 1;
    }
    return DEC_ERROR(ErrInvalid, "NULL where null is not allowed");
  }

  // validate string literal and calculate its decoded length
  usize enclen = DEC_DATA_AVAIL, declen;
  err_t err = co_strlit_check(p, &enclen, &declen);
  if UNLIKELY(err)
    goto end_invalid;

  // allocate memory for string (+1 for NUL terminator)
  u8* dst = mem_alloc(d->ast_ma, declen + 1).p;
  if UNLIKELY(!dst)
    return DEC_ERROR(ErrNoMem, "out of memory");
  *dstp = dst;
  dst[declen] = 0;

  if (enclen - 2 == declen) {
    // copy verbatim string
    memcpy(dst, p + 1, declen);
  } else {
    // decode string with escape sequence
    if UNLIKELY(( err = co_strlit_decode(p, enclen, dst, declen) ))
      goto end_invalid;
  }
  //dlog("decoded string:\n————————\n%.*s\n———————", (int)declen, (const char*)dst);
  p += enclen;
  return p;
end_invalid:
  p += enclen;
  return DEC_ERROR(err, "invalid string literal");
}


static const u8* dec_nodearray1(DEC_PARAMS, nodearray_t* dstp, memalloc_t ma) {
  // nodearray = "*" len (SP u32x){len}
  if UNLIKELY(DEC_DATA_AVAIL < 2 || *p != '*')
    return DEC_ERROR(ErrInvalid, "expected '*N'");
  p++; // consume '*'

  // read array length
  u32 len = 0;
  p = dec_u32x(DEC_ARGS, &len);
  if UNLIKELY((usize)len > USIZE_MAX / sizeof(void*))
    return DEC_ERROR(ErrOverflow, "node array too large (%u)", len);

  if (len == 0) {
    dstp->len = 0;
    dstp->cap = 0;
    return p;
  }

  // allocate memory for array
  mem_t m = mem_alloc(ma, (usize)len * sizeof(void*));
  if UNLIKELY(m.p == NULL)
    return DEC_ERROR(ErrNoMem, "mem_alloc %zu B", (usize)len * sizeof(void*));
  node_t** v = m.p;

  // read (SP u32x){len}
  for (u32 i = 0, id; i < len; ++i) {
    p = dec_byte(DEC_ARGS, ' ');
    p = dec_u32x(DEC_ARGS, &id);
    if (d->err)
      break;
    if UNLIKELY(id > d->nodecount)
      return DEC_ERROR(ErrInvalid, "invalid node ID 0x%x", id);
    v[i] = assertnotnull(d->nodetab[id]);
  }

  if (d->err)
    return p;

  dstp->v = v;
  dstp->cap = m.size / sizeof(void*);
  dstp->len = len;

  return p;
}


static const u8* dec_nodearray(DEC_PARAMS, nodearray_t* dstp) {
  return dec_nodearray1(DEC_ARGS, dstp, d->ast_ma);
}


/*static const u8* dec_nodelist(DEC_PARAMS, u16 listoffs, void** listhead) {
  // same encoding as nodearray
  d->tmpnodearray.len = 0;
  p = dec_nodearray1(DEC_ARGS, &d->tmpnodearray, d->ma);

  if (d->tmpnodearray.len == 0) {
    *listhead = NULL;
    return p;
  }

  for (u32 i = 0, lasti = d->tmpnodearray.len - 1; ; i++) {
    void* entry = d->tmpnodearray.v[i];
    if (i == 0)
      *listhead = entry;
    // advance entry to its "next" field
    entry += listoffs;
    if (i == lasti) {
      *(void**)entry = NULL;
      break;
    }
    *(void**)entry = d->tmpnodearray.v[i + 1];
  }

  return p;
}*/


static const u8* decode_field(DEC_PARAMS, void* fp, ast_field_t f) {
  // dlog("  %-12s %-9s  +%u", f.name, ast_fieldtype_str(f.type), f.offs);

  // set fp to point to the field of the node_t
  fp += f.offs;

  p = dec_byte(DEC_ARGS, ' ');

  switch ((enum ast_fieldtype)f.type) {
  case AST_FIELD_U8:        return dec_u8x(DEC_ARGS, fp);
  case AST_FIELD_U16:       return dec_u16x(DEC_ARGS, fp);
  case AST_FIELD_U32:       return dec_u32x(DEC_ARGS, fp);
  case AST_FIELD_U64:       return dec_u64x(DEC_ARGS, fp);
  case AST_FIELD_F64:       return dec_f64x(DEC_ARGS, fp);
  case AST_FIELD_LOC:       return dec_loc(DEC_ARGS, fp);
  case AST_FIELD_SYM:       return dec_symref(DEC_ARGS, fp, /*allow_null*/false); break;
  case AST_FIELD_SYMZ:      return dec_symref(DEC_ARGS, fp, /*allow_null*/true); break;
  case AST_FIELD_NODE:      return dec_noderef(DEC_ARGS, fp, /*allow_null*/false); break;
  case AST_FIELD_NODEZ:     return dec_noderef(DEC_ARGS, fp, /*allow_null*/true); break;
  case AST_FIELD_STR:       return dec_str(DEC_ARGS, fp, /*allow_null*/false); break;
  case AST_FIELD_STRZ:      return dec_str(DEC_ARGS, fp, /*allow_null*/true); break;
  case AST_FIELD_NODEARRAY: return dec_nodearray(DEC_ARGS, fp); break;
  //case AST_FIELD_NODELIST:  return dec_nodelist(DEC_ARGS, f.listoffs, fp); break;
  case AST_FIELD_UNDEF:     UNREACHABLE; break;
  }

  return p;
}

#ifdef XXXXX
typedef struct astdecoder_ {
  u32         version;
  u32         symcount;    // length of symtab
  u32         nodecount;   // length of nodetab
  u32         rootcount;   // number of root nodes at the tail of nodetab
  u32         srccount;    // length of srctab
  u32         importcount; //
  sym_t*      symtab;      // ID => sym_t
  node_t**    nodetab;     // ID => node_t*
  u32*        srctab;      // ID => srcfileid (document local ID => global ID)
  memalloc_t  ma;
  memalloc_t  ast_ma;
  locmap_t*   locmap;
  const char* srcname;
  const u8*   pstart;
  const u8*   pend;
  const u8*   pcurr;
  err_t       err;
} astdecoder_t;
#endif


static const u8* decode_header(DEC_PARAMS) {
  // check that header is reasonably long
  const usize kMinHeaderSize = strlen("XXXX 0 0 0 0\n");
  if (DEC_DATA_AVAIL < kMinHeaderSize) {
    dlog("header too small");
    d->err = ErrInvalid;
    return p;
  }

  // verify magic
  if UNLIKELY(memcmp(p, FILE_MAGIC, 4) != 0 || p[4] != ' ') {
    if (p[4] != ' ') {
      dlog("missing SP after header magic");
    } else {
      dlog("invalid header magic: %02x%02x%02x%02x", p[0], p[1], p[2], p[3]);
    }
    d->err = ErrInvalid;
    return p;
  }
  p += 4+1;

  p = dec_u32x(DEC_ARGS, &d->version);
  p = dec_whitespace(DEC_ARGS);

  if (d->version != AST_ENC_VERSION && !d->err) {
    dlog("unsupported version: %u", d->version);
    d->err = ErrNotSupported;
  }

  p = dec_u32x(DEC_ARGS, &d->srccount);
  p = dec_whitespace(DEC_ARGS);
  p = dec_u32x(DEC_ARGS, &d->importcount);
  p = dec_whitespace(DEC_ARGS);
  p = dec_u32x(DEC_ARGS, &d->symcount);
  p = dec_whitespace(DEC_ARGS);
  p = dec_u32x(DEC_ARGS, &d->nodecount);
  p = dec_whitespace(DEC_ARGS);
  p = dec_u32x(DEC_ARGS, &d->rootcount);
  p = dec_byte(DEC_ARGS, '\n');

  if (d->rootcount > d->nodecount && !d->err) {
    dlog("invalid rootcount: %u", d->rootcount);
    d->err = ErrInvalid;
  }
  return p;
}


static const u8* decode_bytes_untilchar(
  DEC_PARAMS, const char** startp, usize* lenp, char endc)
{
  // decode endc-terminated line (consumes terminating endc)
  *startp = (const char*)p;
  while (p < pend && *p != endc)
    p++;
  if UNLIKELY(p == pend) {
    *lenp = 0;
    return DEC_ERROR(ErrInvalid);
  }
  *lenp = (usize)(uintptr)(p - *(const u8**)startp);
  return p + 1;
}


static const u8* decode_pkg(DEC_PARAMS, pkg_t* pkg) {
  // pkg = pkgroot ":" pkgpath (":" sha256x)? LF
  const char* linep;
  usize linelen;
  bool ok = true;

  // pkg.root
  p = decode_bytes_untilchar(DEC_ARGS, &linep, &linelen, ':');
  if UNLIKELY(
    pkg->root.len > 0 && coverbose &&
    (pkg->root.len != linelen || memcmp(pkg->root.p, linep, linelen) != 0) )
  {
    elog("[astdecoder] warning: %s: unexpected pkg root \"%.*s\" (expected \"%s\")",
      relpath(d->srcname), (int)linelen, linep, pkg->root.p);
  }
  pkg->root.len = 0;
  ok &= str_appendlen(&pkg->root, linep, linelen);

  // pkg.path
  p = decode_bytes_untilchar(DEC_ARGS, &linep, &linelen, '\n');

  // decode optional (":" sha256x)?
  isize coloni = string_lastindexof(linep, linelen, ':');
  if (coloni > -1) {
    const u8* hex = (u8*)linep + (coloni + 1);
    usize taillen = linelen - (usize)coloni - 1;
    if (taillen != 64)
      return DEC_ERROR(ErrInvalid, "invalid pkg api hash len (%zu)", taillen);
    u8* api_sha256_bytes = (u8*)&pkg->api_sha256;
    for (usize i = 0; i < 32; i++) {
      #define DEC_HEXDIGIT(x) ( \
        ((x) - '0') - \
        ( (u8)('a' <= (x) && (x) <= 'f') * (('a' - 10) - '0') ) - \
        ( (u8)('A' <= (x) && (x) <= 'F') * (('A' - 10) - '0') ) \
      )
      u8 a = hex[i*2];
      u8 b = hex[i*2 + 1];
      api_sha256_bytes[i] = (DEC_HEXDIGIT(a) << 4) | DEC_HEXDIGIT(b);
    }
    linelen = (usize)coloni;
  }

  // check pkg.path
  if UNLIKELY(
    pkg->path.len > 0 &&
    (pkg->path.len != linelen || memcmp(pkg->path.p, linep, linelen) != 0) )
  {
    d->err = ErrInvalid;
    if (coverbose) {
      elog("[astdecoder] error: %s: unexpected pkg path \"%.*s\" (expected \"%s\")",
        relpath(d->srcname), (int)linelen, linep, pkg->path.p);
    }
    return p;
  }
  pkg->path.len = 0;
  ok &= str_appendlen(&pkg->path, linep, linelen);

  // pkg.dir
  pkg->dir.len = 0;
  ok &= pkg_dir_of_root_and_path(&pkg->dir, str_slice(pkg->root), str_slice(pkg->path));

  if UNLIKELY(!ok)
    return DEC_ERROR(ErrNoMem, "OOM");
  return p;
}


static const u8* decode_srcfiles(DEC_PARAMS, pkg_t* pkg) {
  if (d->srccount == 0)
    return p;

  // allocate memory for srctab
  usize nbyte = (usize)d->srccount;
  if (check_mul_overflow(nbyte, sizeof(*d->srctab), &nbyte)) {
    d->err = ErrOverflow;
    return p;
  }
  void* srctab = mem_alloc(d->ma, nbyte).p;
  if UNLIKELY(!srctab) {
    d->err = ErrNoMem;
    return srctab;
  }
  d->srctab = srctab;

  for (u32 i = 0; i < d->srccount; i++) {
    const char* linep;
    usize linelen;
    p = decode_bytes_untilchar(DEC_ARGS, &linep, &linelen, '\n');

    // allocate or retrieve srcfile for pkg
    srcfile_t* srcfile = pkg_add_srcfile(pkg, linep, linelen, NULL);
    if UNLIKELY(!srcfile)
      return DEC_ERROR(ErrNoMem, "pkg_add_srcfile OOM");

    // intern in locmap, resulting in our "local" srcfileid
    u32 srcfileid = locmap_intern_srcfileid(&d->c->locmap, srcfile, d->ma);
    if UNLIKELY(srcfileid == 0)
      return DEC_ERROR(ErrNoMem, "locmap_srcfileid OOM");

    // dlog("srctab[%u] = srcfile #%u (\"%s\")", i, srcfileid, srcfile->name.p);
    d->srctab[i] = srcfileid;
  }
  return p;
}


static const u8* decode_imports(DEC_PARAMS, pkg_t* pkg, sha256_t* api_sha256v) {
  pkg_t tmp = {0};

  for (u32 i = 0; i < d->importcount; i++) {
    tmp.dir.len = 0;
    tmp.root.len = 0;
    tmp.path.len = 0;
    p = decode_pkg(DEC_ARGS, &tmp);
    if (d->err)
      break;

    memcpy(&api_sha256v[i], &tmp.api_sha256, sizeof(*api_sha256v));

    // Note: we do NOT check if the package actually exists.
    // That is left for pkgbuild to do as it loads the package.

    // resolve package in compiler's pkgindex
    pkg_t* dep;
    err_t err = pkgindex_intern(
      d->c, str_slice(tmp.dir), str_slice(tmp.path), &tmp.api_sha256, &dep);

    // add dep to pkg->imports
    if (!err && !pkg_imports_add(pkg, dep, d->c->ma))
      err = ErrNoMem;

    if UNLIKELY(err) {
      dlog("OOM");
      p = pend;
      d->err = err;
      break;
    }
  }

  str_free(tmp.dir);
  str_free(tmp.root);
  str_free(tmp.path);

  return p;
}


static const u8* decode_symtab(DEC_PARAMS) {
  for (u32 i = 0; i < d->symcount; i++) {
    const u8* start = p;
    while (*p != '\n' && p < pend)
      p++;
    if UNLIKELY(p == pend)
      return DEC_ERROR(ErrInvalid);
    sym_t sym = sym_intern((const char*)start, (usize)(uintptr)(p - start));
    // dlog("symtab[%u] = \"%s\"", i, sym);
    d->symtab[i] = sym;
    p++; // consume '\n'
  }
  return p;
}


static const u8* decode_universal_node(DEC_PARAMS, u32 node_id, nodekind_t kind) {
  node_t* n = NULL;
  switch (kind) {
    case TYPE_VOID:    n = (node_t*)type_void; break;
    case TYPE_BOOL:    n = (node_t*)type_bool; break;
    case TYPE_INT:     n = (node_t*)type_int; break;
    case TYPE_UINT:    n = (node_t*)type_uint; break;
    case TYPE_I8:      n = (node_t*)type_i8; break;
    case TYPE_I16:     n = (node_t*)type_i16; break;
    case TYPE_I32:     n = (node_t*)type_i32; break;
    case TYPE_I64:     n = (node_t*)type_i64; break;
    case TYPE_U8:      n = (node_t*)type_u8; break;
    case TYPE_U16:     n = (node_t*)type_u16; break;
    case TYPE_U32:     n = (node_t*)type_u32; break;
    case TYPE_U64:     n = (node_t*)type_u64; break;
    case TYPE_F32:     n = (node_t*)type_f32; break;
    case TYPE_F64:     n = (node_t*)type_f64; break;
    case TYPE_UNKNOWN: n = (node_t*)type_unknown; break;
    default:
      assertf(0, "unexpected node kind %s", nodekind_name(kind));
      UNREACHABLE;
  }

  // dlog("nodetab[%u] = (universal node of kind %s)", node_id, nodekind_name(n->kind));
  d->nodetab[node_id] = n;

  // read line feed
  p = dec_byte(DEC_ARGS, '\n');

  return p;
}


static const u8* decode_node(DEC_PARAMS, u32 node_id) {
  const usize kMinEncNodeSize = strlen("XXXX 0 0 0\n");
  if UNLIKELY(DEC_DATA_AVAIL < kMinEncNodeSize)
    return DEC_ERROR(ErrInvalid), pend;

  // read kind (4 bytes interpreted as u32be)
  u32 kindid = (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24);
  p += 4;
  nodekind_t kind = nodekind_of_tag(kindid);
  if UNLIKELY(kind == NODE_BAD)
    return DEC_ERROR(ErrInvalid, "invalid node kind '%.4s'", p), pend;

  // get field table for kind
  const ast_field_t* fieldtab = g_ast_fieldtab[kind];

  // intercept universal types, singletons compared by address,
  // by looking for nodes that are represented solely by type_t
  if (fieldtab == g_fieldsof_type_t)
    return decode_universal_node(DEC_ARGS, node_id, kind);

  // decode standard node, starting by allocating memory for the node struct
  usize nodesize = g_ast_sizetab[kind];
  node_t* n = mem_alloc_zeroed(d->ast_ma, nodesize).p;
  if UNLIKELY(!n)
    return DEC_ERROR(ErrNoMem), pend;
  d->nodetab[node_id] = n;
  n->kind = kind;

  // read flags
  p = dec_whitespace(DEC_ARGS);
  p = dec_u16x(DEC_ARGS, &n->flags);
  if (n->flags & ~NODEFLAGS_ALL)
    dlog("scrubbed invalid nodeflags %x from %x", n->flags & ~NODEFLAGS_ALL, n->flags);
  n->flags = n->flags & NODEFLAGS_ALL; // scrub away invalid flags

  // read nuse
  p = dec_whitespace(DEC_ARGS);
  p = dec_u32x(DEC_ARGS, &n->nuse);

  // read loc
  p = dec_whitespace(DEC_ARGS);
  p = dec_loc(DEC_ARGS, &n->loc);

  // read fields
  for (u8 i = 0; i < g_ast_fieldlentab[n->kind]; i++) {
    p = decode_field(DEC_ARGS, n, fieldtab[i]);
    if (d->err)
      return pend;
  }

  // read line feed
  p = dec_byte(DEC_ARGS, '\n');

  // dlog("nodetab[%u] = (kind %s, flags 0x%04x, loc 0x%llx)",
  //   node_id, nodekind_name(n->kind), n->flags, n->loc);

  return p;
}


static const u8* decode_nodes(DEC_PARAMS) {
  for (u32 node_id = 0; node_id < d->nodecount; node_id++) {
    p = decode_node(DEC_ARGS, node_id);
    if (d->err)
      break;
  }
  return p;
}


static bool dec_tmptabs_alloc(astdecoder_t* d) {
  usize nbyte_nodetab = (usize)d->nodecount;
  usize nbyte_symtab = (usize)d->symcount;
  usize nbyte;
  if (check_mul_overflow(nbyte_nodetab, sizeof(*d->nodetab), &nbyte_nodetab) ||
      check_mul_overflow(nbyte_symtab, sizeof(*d->symtab), &nbyte_symtab) ||
      check_add_overflow(nbyte_nodetab, nbyte_symtab, &nbyte) )
  {
    d->err = ErrOverflow;
    return false;
  }
  nbyte = ALIGN2(nbyte, sizeof(void*));

  void* p = mem_alloc(d->ma, nbyte).p;
  if (!p && nbyte > 0) {
    d->err = ErrNoMem;
    return false;
  }

  d->nodetab = p;
  d->symtab = p + nbyte_nodetab;

  return true;
}


static void dec_tmptabs_free(astdecoder_t* d) {
  if (!d->nodetab)
    return;
  usize nbyte = ((usize)d->nodecount * sizeof(*d->nodetab))
              + ((usize)d->symcount * sizeof(*d->symtab))
              + ((usize)d->srccount * sizeof(*d->srctab));
  nbyte = ALIGN2(nbyte, sizeof(void*));
  mem_freex(d->ma, MEM(d->nodetab, nbyte));
}



astdecoder_t* nullable astdecoder_open(
  compiler_t* c,
  memalloc_t  ast_ma,
  const char* srcname,
  const u8*   src,
  usize       srclen)
{
  astdecoder_t* d = mem_alloct(c->ma, astdecoder_t);
  if (d) {
    d->ast_ma = ast_ma;
    d->ma = c->ma;
    d->c = c;
    d->srcname = srcname;
    d->pstart = src;
    d->pend = src + srclen;
    d->pcurr = src;
  }
  return d;
}


void astdecoder_close(astdecoder_t* d) {
  if (d->srctab)
    mem_freex(d->ma, MEM(d->srctab, (usize)d->srccount * sizeof(*d->srctab)));
  dec_tmptabs_free(d);
  nodearray_dispose(&d->tmpnodearray, d->ma);

  mem_freet(d->ma, d);
}


err_t astdecoder_decode_header(astdecoder_t* d, pkg_t* pkg, u32* importcount) {
  // DEC_ARGS
  const u8* p = d->pcurr;
  const u8* pend = d->pend;

  // decode header
  p = decode_header(DEC_ARGS);
  if (d->err) {
    dlog("decode_header: %s", err_str(d->err));
    goto end;
  }

  // dlog("header: symcount %u, nodecount %u, rootcount %u",
  //   d->symcount, d->nodecount, d->rootcount);

  // allocate memory for temporary tables
  if (!dec_tmptabs_alloc(d)) {
    dlog("dec_tmptabs_alloc: %s", err_str(d->err));
    goto end;
  }

  p = decode_pkg(DEC_ARGS, pkg);
  p = decode_srcfiles(DEC_ARGS, pkg);

end:
  d->pcurr = p;
  *importcount = d->importcount;
  return d->err;
}


err_t astdecoder_decode_imports(astdecoder_t* d, pkg_t* pkg, sha256_t* api_sha256v) {
  // DEC_ARGS
  const u8* p = d->pcurr;
  const u8* pend = d->pend;

  d->pcurr = decode_imports(DEC_ARGS, pkg, api_sha256v);

  return d->err;
}


err_t astdecoder_decode_ast(astdecoder_t* d, node_t** resultv[], u32* resultc) {
  assertf(d->version > 0, "header not decoded");

  // DEC_ARGS
  const u8* p = d->pcurr;
  const u8* pend = d->pend;

  node_t** roots = NULL;

  // decode symbols
  p = decode_symtab(DEC_ARGS);
  if (d->err)
    goto error;

  // decode nodes
  p = decode_nodes(DEC_ARGS);
  if (d->err)
    goto error;

  // create list of root nodes, returned to the user via resultv & resultc
  // note: alloc size is guaranteed to not overflow since rootcount <= nodecount.
  roots = mem_alloc(d->ast_ma, (usize)d->rootcount * sizeof(void*)).p;
  if (!roots) {
    d->err = ErrNoMem;
    goto error;
  }

  // read root node IDs
  for (u32 i = 0, id; i < d->rootcount; i++) {
    p = dec_u32x(DEC_ARGS, &id);
    p = dec_byte(DEC_ARGS, '\n');
    if (d->err)
      goto error;
    if UNLIKELY(id > d->nodecount) {
      dlog("invalid root %u; no such node", id);
      d->err = ErrInvalid;
      goto error;
    }
    roots[i] = d->nodetab[id];
  }

  // success
  *resultv = roots;
  *resultc = d->rootcount;
  assert(d->err == 0);
  goto end;

error:
  assert(d->err != 0);
  if (roots)
    mem_freex(d->ast_ma, MEM(roots, (usize)d->rootcount * sizeof(void*)));
  *resultv = NULL;
  *resultc = 0;
end:
  return d->err;
}
