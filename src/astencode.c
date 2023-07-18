// SPDX-License-Identifier: Apache-2.0
/*

AST encoding format:

  root = header
         srcinfo
         symbol{symcount}
         node{nodecount}
         nodeid{rootcount}

  header    = magic SP
              version SP
              symcount SP
              nodecount SP
              rootcount LF
  magic     = "cAST"
  version   = u32x
  symcount  = u32x
  nodecount = u32x
  rootcount = u32x

  srcinfo   = pkgpath? LF
              srcdir? LF
              srccount LF (srcfile LF){srccount}
              importcount LF (pkgpath LF){importcount}
  pkgpath   = filepath
  srcdir    = filepath
  srccount  = u32x
  srcfile   = filepath
  mtime     = u64x

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
#include "compiler.h"
#include "astencode.h"
#include "hash.h"


#define FILE_MAGIC "cAST"


// DEBUG_LOG_ENCODE_STATS: define to dlog some encoder stats
//#define DEBUG_LOG_ENCODE_STATS

#if defined(DEBUG_LOG_ENCODE_STATS) && !defined(DEBUG)
  #undef DEBUG_LOG_ENCODE_STATS
#endif

// nodekind_t => u8 sizeof(struct_type)
static const u8 g_nodekindsizetab[NODEKIND_COUNT] = {
  #define _(kind, TYPE, tag, ...) [kind] = sizeof(TYPE),
  FOREACH_NODEKIND(_)
  #undef _
};

// nodekind_t => u32 tag
static const u32 g_nodekindtagtab[NODEKIND_COUNT] = {
  #define _(kind, TYPE, tag, ...) [kind] = CO_STRu32(tag),
  FOREACH_NODEKIND(_)
  #undef _
};

// u32 tag => nodekind_t
static nodekind_t nodekind_of_tag(u32 tag) {
  switch (tag) {
    #define _(kind, TYPE, tag, ...) \
      case CO_STRu32(tag): return kind;
    FOREACH_NODEKIND(_)
    #undef _
    default: return NODE_BAD;
  }
}


#ifdef DEBUG
UNUSED __attribute__((constructor)) static void check_nodekindtagtab() {
  for (nodekind_t i = 0; i < (nodekind_t)countof(g_nodekindtagtab); i++) {
    assertf(g_nodekindtagtab[i] != 0, "missing g_nodekindtagtab[%s]", nodekind_name(i));
    for (nodekind_t j = 0; j < (nodekind_t)countof(g_nodekindtagtab); j++) {
      assertf(i == j || g_nodekindtagtab[i] != g_nodekindtagtab[j],
        "duplicate id \"%.4s\": [%s] & [%s]",
        (char*)&g_nodekindtagtab[i], nodekind_name(i), nodekind_name(j));
    }
  }
}
#endif

//———————————————————————————————————————————————————————————————————————————————————————

typedef u8 ae_fieldtype_t;
enum ae_fieldtype {
  AFT_UNDEF = (ae_fieldtype_t)0,
  AFT_U8,
  AFT_U16,
  AFT_U32,
  AFT_U64,
  AFT_F64,
  AFT_LOC,               // loc_t
  AFT_SYM,  AFT_SYMZ,  // sym_t, sym_t nullable
  AFT_NODE, AFT_NODEZ, // node_t*, node_t* nullable
  AFT_STR,  AFT_STRZ,  // u8*, u8* nullable
  AFT_NODEARRAY, // nodearray_t
  AFT_CUSTOM,
};

typedef struct {
  u16            offs; // offset in struct
  ae_fieldtype_t type;
  //u8           unused;
  #if DEBUG
    const char* name;
  #endif
} ae_field_t;

#if DEBUG
  #define FIELD(STRUCT_TYPE, field_name, FIELD_TYPE) \
    { offsetof(STRUCT_TYPE, field_name), AFT_##FIELD_TYPE, #field_name }
#else
  #define FIELD(STRUCT_TYPE, field_name, FIELD_TYPE) \
    { offsetof(STRUCT_TYPE, field_name), AFT_##FIELD_TYPE }
#endif

#define EXPR_FIELDS /* expr_t */ \
  FIELD(expr_t, type, NODEZ) \
// end EXPR_FIELDS

#define LOCAL_FIELDS /* local_t */ \
  EXPR_FIELDS, \
  FIELD(local_t, type,    NODEZ), \
  FIELD(local_t, name,    SYM), \
  FIELD(local_t, nameloc, LOC), \
  FIELD(local_t, offset,  U64), \
  FIELD(local_t, init,    NODEZ) \
// end LOCAL_FIELDS

#define TYPE_FIELDS /* type_t */ \
  FIELD(type_t, size,  U64), \
  FIELD(type_t, align, U8), \
  FIELD(type_t, tid,   SYMZ) \
// end TYPE_FIELDS

#define PTRTYPE_FIELDS /* ptrtype_t */ \
  TYPE_FIELDS, \
  FIELD(ptrtype_t, elem, NODE) \
// end PTRTYPE_FIELDS


// per-AST-struct field tables
static const ae_field_t g_fieldsof_node_t[0] = {};
static const ae_field_t g_fieldsof_unit_t[] = {
  FIELD(unit_t, children, NODEARRAY),
  // TODO: srcfile
  // TODO: tfuns
  // TODO: importlist
};
static const ae_field_t g_fieldsof_typedef_t[] = {
  FIELD(typedef_t, type, NODE),
};
static const ae_field_t g_fieldsof_import_t[] = {
  FIELD(import_t, path, STR),
  FIELD(import_t, pathloc, LOC),
  // TODO: idlist
  // TODO: isfrom
  // TODO: next_import
};
static const ae_field_t g_fieldsof_fun_t[] = {
  EXPR_FIELDS,
  FIELD(fun_t, type,        NODEZ),
  FIELD(fun_t, name,        SYMZ),
  FIELD(fun_t, nameloc,     LOC),
  FIELD(fun_t, body,        NODEZ),
  FIELD(fun_t, recvt,       NODEZ),
  FIELD(fun_t, mangledname, STRZ),
  FIELD(fun_t, abi,         U32),
};
static const ae_field_t g_fieldsof_block_t[] = {
  EXPR_FIELDS,
  FIELD(block_t, children, NODEARRAY),
  //TODO: FIELD(block_t, drops, DROPARRAY), // drop_t[]
  FIELD(block_t, endloc, LOC),
};
static const ae_field_t g_fieldsof_call_t[] = {
  EXPR_FIELDS,
  FIELD(call_t, recv, NODE),
  FIELD(call_t, args, NODEARRAY),
  FIELD(call_t, argsendloc, LOC),
};
static const ae_field_t g_fieldsof_typecons_t[] = {
  EXPR_FIELDS,
  // TODO
};
static const ae_field_t g_fieldsof_idexpr_t[] = {
  EXPR_FIELDS,
  FIELD(idexpr_t, name, SYM),
  FIELD(idexpr_t, ref, NODEZ),
};
static const ae_field_t g_fieldsof_local_t[] = {
  EXPR_FIELDS,
  FIELD(local_t, name,    SYM),
  FIELD(local_t, nameloc, LOC),
  FIELD(local_t, offset,  U64),
  FIELD(local_t, init,    NODEZ),
};
static const ae_field_t g_fieldsof_member_t[] = { // member_t
  EXPR_FIELDS,
  FIELD(member_t, recv, NODE),
  FIELD(member_t, name, SYM),
  FIELD(member_t, target, NODEZ),
};
static const ae_field_t g_fieldsof_subscript_t[] = { // subscript_t
  EXPR_FIELDS,
  FIELD(subscript_t, recv, NODE),
  FIELD(subscript_t, index, NODE),
  FIELD(subscript_t, index_val, U64),
  FIELD(subscript_t, endloc, LOC),
};
static const ae_field_t g_fieldsof_unaryop_t[] = {
  EXPR_FIELDS,
  FIELD(unaryop_t, op, U8), // op_t
  FIELD(unaryop_t, expr, NODE),
};
static const ae_field_t g_fieldsof_binop_t[] = {
  EXPR_FIELDS,
  FIELD(binop_t, op, U8), // op_t
  FIELD(binop_t, left, NODE),
  FIELD(binop_t, right, NODE),
};
static const ae_field_t g_fieldsof_ifexpr_t[] = {
  EXPR_FIELDS,
  FIELD(ifexpr_t, cond, NODE),
  FIELD(ifexpr_t, thenb, NODE),
  FIELD(ifexpr_t, elseb, NODEZ),
};
static const ae_field_t g_fieldsof_forexpr_t[] = {
  EXPR_FIELDS,
  FIELD(forexpr_t, start, NODEZ),
  FIELD(forexpr_t, cond, NODE),
  FIELD(forexpr_t, body, NODE),
  FIELD(forexpr_t, end, NODEZ),
};
static const ae_field_t g_fieldsof_retexpr_t[] = {
  EXPR_FIELDS,
  FIELD(retexpr_t, value, NODEZ),
};
static const ae_field_t g_fieldsof_intlit_t[] = {
  EXPR_FIELDS,
  FIELD(intlit_t, intval, U64),
};
static const ae_field_t g_fieldsof_floatlit_t[] = {
  EXPR_FIELDS,
  FIELD(floatlit_t, f64val, F64),
};
static const ae_field_t g_fieldsof_strlit_t[] = {
  EXPR_FIELDS,
  FIELD(strlit_t, bytes, STR),
  FIELD(strlit_t, len, U64),
};
static const ae_field_t g_fieldsof_arraylit_t[] = {
  EXPR_FIELDS,
  FIELD(arraylit_t, endloc, LOC),
  FIELD(arraylit_t, values, NODEARRAY),
};

static const ae_field_t g_fieldsof_type_t[] = {
  TYPE_FIELDS,
};
static const ae_field_t g_fieldsof_arraytype_t[] = {
  TYPE_FIELDS,
};
static const ae_field_t g_fieldsof_funtype_t[] = {
  TYPE_FIELDS,
};
static const ae_field_t g_fieldsof_ptrtype_t[] = {
  PTRTYPE_FIELDS,
};
static const ae_field_t g_fieldsof_reftype_t[] = {
  PTRTYPE_FIELDS,
};
static const ae_field_t g_fieldsof_slicetype_t[] = {
  PTRTYPE_FIELDS,
  FIELD(slicetype_t, endloc, LOC),
};
static const ae_field_t g_fieldsof_opttype_t[] = {
  PTRTYPE_FIELDS,
};
static const ae_field_t g_fieldsof_structtype_t[] = {
  TYPE_FIELDS,
  FIELD(structtype_t, name,        SYMZ),
  FIELD(structtype_t, mangledname, STRZ),
  FIELD(structtype_t, fields,      NODEARRAY),
};
static const ae_field_t g_fieldsof_aliastype_t[] = {
  TYPE_FIELDS,
  FIELD(aliastype_t, name,        SYM),
  FIELD(aliastype_t, elem,        NODE),
  FIELD(aliastype_t, mangledname, STRZ),
};
static const ae_field_t g_fieldsof_unresolvedtype_t[] = {
  TYPE_FIELDS,
  FIELD(unresolvedtype_t, name, SYM),
  FIELD(unresolvedtype_t, resolved, NODEZ),
};

#undef FIELD
#undef EXPR_FIELDS
#undef LOCAL_FIELDS
#undef TYPE_FIELDS
#undef PTRTYPE_FIELDS

// g_fieldtab maps nodekind_t => ae_field_t[]
static const ae_field_t* g_fieldtab[NODEKIND_COUNT] = {
  #define _(kind, TYPE, tag, ...) [kind] = g_fieldsof_##TYPE,
  FOREACH_NODEKIND(_)
  #undef _
};

// g_fieldlentab maps nodekind_t => countof(ae_field_t[])
static const u8 g_fieldlentab[NODEKIND_COUNT] = {
  #define _(kind, TYPE, tag, ...) [kind] = countof(g_fieldsof_##TYPE),
  FOREACH_NODEKIND(_)
  #undef _
};


UNUSED static const char* ae_fieldtype_str(ae_fieldtype_t t) {
  switch ((enum ae_fieldtype)t) {
    case AFT_UNDEF:     return "UNDEF";
    case AFT_U8:        return "U8";
    case AFT_U16:       return "U16";
    case AFT_U32:       return "U32";
    case AFT_U64:       return "U64";
    case AFT_F64:       return "F64";
    case AFT_LOC:       return "LOC";
    case AFT_SYM:       return "SYM";
    case AFT_SYMZ:      return "SYMZ";
    case AFT_NODE:      return "NODE";
    case AFT_NODEZ:     return "NODEZ";
    case AFT_STR:       return "STR";
    case AFT_STRZ:      return "STRZ";
    case AFT_NODEARRAY: return "NODEARRAY";
    case AFT_CUSTOM:    return "CUSTOM";
  }
  return "??";
}

// Conversion of f64 <-> u64
// Currently assumes f64 is represented as IEEE 754 binary64
inline static u64 f64_to_u64(f64 v) { return *(u64*)&v; }
inline static f64 u64_to_f64(u64 v) { return *(f64*)&v; }


//———————————————————————————————————————————————————————————————————————————————————————
// astencode_t implementation


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


static u32 encoded_sym_index(const astencode_t* a, sym_t sym) {
  u32 index;
  UNUSED sym_t* vp = array_sortedset_lookup(
    sym_t, &a->symmap, &sym, &index, (array_sorted_cmp_t)ptr_cmp, NULL);
  assertf(vp != NULL, "%s not in symmap", sym);
  return index;
}


static u32 encoded_node_index(const astencode_t* a, const void* np) {
  uintptr* vp = (uintptr*)map_lookup_ptr(&a->nodemap, np);
  assertf(vp != NULL, "node %p %s not in nodemap",
    np, nodekind_name(((node_t*)np)->kind));
  assertf(*vp != 0, "nodemap entry for %p %s is 0",
    np, nodekind_name(((node_t*)np)->kind));
  // note: node indices are stored in nodemap with +1 larger value
  // to keep 0 reserved for "not in map"
  return (u32)(*vp - 1);
}


#define AST_ENC_VERSION 1u


static void encode_header(astencode_t* a, buf_t* outbuf) {
  // write header
  char* p = outbuf->chars + outbuf->len;
  memcpy(p, FILE_MAGIC, 4); p += 4; *p++ = ' ';
  p += fmt_u64_base16(p, 8, AST_ENC_VERSION); *p++ = ' ';
  p += fmt_u64_base16(p, 8, a->symmap.len); *p++ = ' ';
  p += fmt_u64_base16(p, 8, a->nodelist.len); *p++ = ' ';
  p += fmt_u64_base16(p, 8, a->rootlist.len);
  *p++ = '\n';
  buf_setlenp(outbuf, p);
}


static void encode_filepath(astencode_t* a, buf_t* outbuf, const char* str, usize len) {
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


static void encode_srcinfo(astencode_t* a, buf_t* outbuf) {
  if (a->srcfileids.len == 0) {
    a->oom |= !buf_print(outbuf, "\n\n0\n");
    return;
  }

  pkg_t* pkg = NULL;
  usize nbyte = ndigits16(a->srcfileids.len) + 1; // srccount LF

  // find pkg and calculate outbuf size needed to encode srcinfo
  for (u32 enc_srcfileid = 0; enc_srcfileid < a->srcfileids.len; enc_srcfileid++) {
    u32 srcfileid = a->srcfileids.v[enc_srcfileid];
    srcfile_t* srcfile = locmap_srcfile(a->locmap, srcfileid);
    assertf(srcfile, "srcfile#%u not in locmap", srcfileid);

    // check so that there's just one package
    #ifdef CO_SAFE
      if (pkg) {
        assert(srcfile->pkg != NULL);
        safecheckf(pkg == srcfile->pkg,
          "srcfiles from mixed packages %s (%p) and %s (%p)",
          pkg->path.p, pkg, srcfile->pkg->path.p, srcfile->pkg);
      }
    #endif
    pkg = assertnotnull(srcfile->pkg);

    // srcfile LF
    a->oom |= check_add_overflow(nbyte, srcfile->name.len + 1, &nbyte);
  }

  // pkgpath LF srcdir LF
  a->oom |= check_add_overflow(nbyte, pkg->path.len + 1, &nbyte);
  a->oom |= check_add_overflow(nbyte, pkg->dir.len + 1, &nbyte);

  // importcount LF (pkgpath LF){importcount}
  a->oom |= check_add_overflow(nbyte, (usize)ndigits16(pkg->imports.len) + 1, &nbyte);
  for (u32 i = 0; i < pkg->imports.len; i++) {
    const pkg_t* dep = pkg->imports.v[i];
    a->oom |= check_add_overflow(nbyte, (usize)ndigits16(dep->path.len) + 1, &nbyte);
  }

  if (a->oom)
    return;
  BUF_RESERVE(nbyte);

  // pkgpath LF
  encode_filepath(a, outbuf, pkg->path.p, pkg->path.len);
  outbuf->chars[outbuf->len++] = '\n';

  // srcdir LF
  encode_filepath(a, outbuf, pkg->dir.p, pkg->dir.len);
  outbuf->chars[outbuf->len++] = '\n';

  // srccount LF (srcfile LF){srccount}
  outbuf->len += fmt_u64_base16(outbuf->chars + outbuf->len, 8, a->srcfileids.len);
  outbuf->chars[outbuf->len++] = '\n';
  for (u32 enc_srcfileid = 0; enc_srcfileid < a->srcfileids.len; enc_srcfileid++) {
    srcfile_t* srcfile = locmap_srcfile(a->locmap, a->srcfileids.v[enc_srcfileid]);
    encode_filepath(a, outbuf, srcfile->name.p, srcfile->name.len);
    assert(buf_avail(outbuf) > 0);
    outbuf->chars[outbuf->len++] = '\n';
  }

  // importcount LF (pkgpath LF){importcount}
  outbuf->len += fmt_u64_base16(outbuf->chars + outbuf->len, 8, pkg->imports.len);
  outbuf->chars[outbuf->len++] = '\n';
}


static void encode_syms(astencode_t* a, buf_t* outbuf) {
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


static void encode_str(astencode_t* a, buf_t* outbuf, const char* str, usize len) {
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
  astencode_t* a, buf_t* outbuf, const void* fp, ae_field_t f)
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


static void encode_field_str(
  astencode_t* a, buf_t* outbuf, const void* fp, ae_field_t f)
{
  const char* str = *(const char**)fp;
  usize len = strlen(str);
  encode_str(a, outbuf, str, len);
}


static void encode_field_custom(
  astencode_t* a, buf_t* outbuf, const void* fp, ae_field_t f)
{
  dlog("TODO %s", __FUNCTION__);
}


static int u32_cmp(const u32* a, const u32* b, void* ctx) {
  return *a == *b ? 0 : *a < *b ? -1 : 1;
}


static loc_t enc_remap_loc(astencode_t* a, loc_t loc) {
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
static usize field_encsize(astencode_t* a, const void* fp, ae_field_t f) {
  usize z = 1; // leading SP
again:
  switch ((enum ae_fieldtype)f.type) {
    case AFT_NODEZ:
    case AFT_SYMZ:
    case AFT_STRZ:
      if (*(void**)fp == NULL) return z + 1; // "_"
      f.type--; // e.g. AFT_NODEZ -> AFT_NODE
      goto again;

    case AFT_U8:   return z + ndigits16(*(u8*)fp);
    case AFT_U16:  return z + ndigits16(*(u16*)fp);
    case AFT_U32:  return z + ndigits16(*(u32*)fp);
    case AFT_U64:  return z + ndigits16(*(u64*)fp);
    case AFT_F64:  return z + 16; // TODO FIXME
    case AFT_LOC:  return z + ndigits16(*(loc_t*)fp);
    case AFT_SYM:
      return z + 1 + ndigits16(encoded_sym_index(a, *(sym_t*)fp)); // "#" u32x
    case AFT_NODE:
      return z + 1 + ndigits16(encoded_node_index(a, *(node_t**)fp)); // "&" u32x

    case AFT_STR: { // '"' strdata '"'
      // optimistic guess: the string does not need escaping
      usize len = strlen(*(const char**)fp);
      //return z + 3ul + ndigits16(len) + len; // e.g. "B hello world"
      return z + 2 + len; // e.g. "hello world"
    }

    case AFT_NODEARRAY: { // "*" len (SP u32x){len}
      // conservative guess: node indices are max, e.g. *2 FFFFFFFF FFFFFFFF
      // This trades some memory slack for a simpler implementation
      const nodearray_t* na = fp;
      return z + 1ul + ndigits16(na->len) + (na->len * (1 + 9));
    }

    case AFT_CUSTOM:
      // can't guess; will have to check bounds when encoding
      return z;

    case AFT_UNDEF:
      break;
  }
  UNREACHABLE;
  return z;
}


static void encode_field(astencode_t* a, buf_t* outbuf, const void* fp, ae_field_t f) {
  //dlog("  %-12s %-9s  +%u", f.name, ae_fieldtype_str(f.type), f.offs);

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

  switch ((enum ae_fieldtype)f.type) {
    case AFT_U8:        u64val = *(u8*)fp; goto enc_u64x;
    case AFT_U16:       u64val = *(u16*)fp; goto enc_u64x;
    case AFT_U32:       u64val = *(u32*)fp; goto enc_u64x;
    case AFT_U64:       u64val = *(u64*)fp; goto enc_u64x;
    case AFT_F64:       u64val = f64_to_u64(*(f64*)fp); goto enc_u64x;
    case AFT_LOC:       u64val = enc_remap_loc(a, *(loc_t*)fp); goto enc_u64x;
    case AFT_SYM:       goto enc_sym;
    case AFT_SYMZ:      if (*(void**)fp) goto enc_sym; goto enc_none;
    case AFT_NODE:      goto enc_node;
    case AFT_NODEZ:     if (*(void**)fp) goto enc_node; goto enc_none;
    case AFT_STR:       goto enc_str;
    case AFT_STRZ:      if (*(void**)fp) goto enc_str; goto enc_none;
    case AFT_NODEARRAY: MUSTTAIL return encode_field_nodearray(a, outbuf, fp, f);
    case AFT_CUSTOM:    MUSTTAIL return encode_field_custom(a, outbuf, fp, f);
    case AFT_UNDEF:     break;
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


static void encode_node(astencode_t* a, buf_t* outbuf, const node_t* n) {
  assertf(n->kind < NODEKIND_COUNT, "%s %u", nodekind_name(n->kind), n->kind);

  // Reserve enough space for base node attributes (kind, flags etc) and fields.
  // Note that astencode_encode has preallocated memory already so in most cases
  // this is just a check on "buf_avail(outbuf) >= NODE_BASE_ENCSIZE" that passes.
  // Only in cases where a previous node wrote long string data does this cause
  // outbuf to grow.
  BUF_RESERVE(NODE_BASE_ENCSIZE);

  // get pointer to outbuf memory
  u8* p = outbuf->bytes + outbuf->len;

  // kind
  memcpy(p, &g_nodekindtagtab[n->kind], 4); p += 4;

  // get field table for kind
  const ae_field_t* fieldtab = g_fieldtab[n->kind];

  // check for universal type
  if (fieldtab == g_fieldsof_type_t) {
    // universal type is encoded solely by kind
    buf_setlenp(outbuf, p);
  } else {
    // it's a standard node, not a universal one

    // base attributes of node_t
    *p++ = '\t'; p += fmt_u64_base16((char*)p, 4, (u64)n->flags);
    *p++ = '\t'; p += fmt_u64_base16((char*)p, 8, (u64)n->nuse);
    *p++ = '\t'; p += fmt_u64_base16((char*)p, 16, (u64)enc_remap_loc(a, n->loc));

    // update outbuf->len
    buf_setlenp(outbuf, p);

    // encode fields
    for (u8 i = 0; i < g_fieldlentab[n->kind]; i++) {
      encode_field(a, outbuf, n, fieldtab[i]);
      if (a->oom)
        return;
    }
  }

  // add terminating LF (encode_field ensures that there's space for it in outbuf)
  assert(buf_avail(outbuf) > 0);
  outbuf->chars[outbuf->len++] = '\n';
}


static usize enc_preallocsize(astencode_t* a) {
  // start with space needed for the header
  usize nbyte = 4 + 1  // magic SP
              + 8 + 1  // version SP
              + 8 + 1  // symcount SP
              + 8 + 1  // nodecount SP
              + 8 + 1  // rootcount LF
  ;

  // approximate space needed for srcinfo
  usize nbyte_srcinfo = 32    // pkgpath (31 chars + LF)
                      + 64    // srcdir (63 chars + LF)
                      + 8 + 1 // srccount (u32x)
                      + (usize)a->srcfileids.len * 16 // srcfile (15 chars + LF)
  ;
  if ((usize)a->srcfileids.len > USIZE_MAX/16 ||
      check_add_overflow(nbyte, nbyte_srcinfo, &nbyte))
  {
    return USIZE_MAX;
  }

  // add space needed to encode symbols
  // nbyte += a->symsize
  if (check_add_overflow(nbyte, a->symsize, &nbyte))
    return USIZE_MAX;

  // approximate space needed to encode nodes in nodelist
  // nbyte_nodes = NODE_BASE_ENCSIZE * nodelist.len
  usize nbyte_nodes;
  if (check_mul_overflow(NODE_BASE_ENCSIZE, (usize)a->nodelist.len, &nbyte_nodes))
    return USIZE_MAX;

  // add approximate space needed to encode nodes
  // nbyte = align4(nbyte) + nbyte_nodes
  if (check_add_overflow(nbyte, nbyte_nodes, &nbyte))
    return USIZE_MAX;

  // calculate space needed to encode root node IDs ("XXXXXXXX\n")
  // nbyte_rootids = 9 * rootlist.len
  usize nbyte_rootids;
  usize maxdigits = ndigits16(a->nodelist.len - 1) + 1; // +1 for '\n'
  if (check_mul_overflow(maxdigits, (usize)a->rootlist.len, &nbyte_rootids))
    return USIZE_MAX;

  // add space needed to encode root node IDs
  if (check_add_overflow(nbyte, nbyte_rootids, &nbyte))
    return USIZE_MAX;

  // align to pointer size, since that is what the allocator will do anyways
  nbyte = ALIGN2(nbyte, sizeof(void*));

  assert(nbyte < USIZE_MAX); // catch false negatives of overflow signalling
  return nbyte;
}


err_t astencode_encode(astencode_t* a, buf_t* outbuf) {
  // allocate space in outbuf
  usize nbyte = enc_preallocsize(a);
  if (nbyte == USIZE_MAX)
    return ErrOverflow;
  #ifdef DEBUG_LOG_ENCODE_STATS
    dlog("%s: allocating %zu B in outbuf (%u syms, %u nodes, %u roots)",
      __FUNCTION__, nbyte, a->symmap.len, a->nodelist.len, a->rootlist.len);
    usize debug_outbuf_initlen = outbuf->len;
  #endif
  BUF_RESERVE(nbyte, ErrNoMem);

  // write header
  encode_header(a, outbuf);

  // write srcinfo
  encode_srcinfo(a, outbuf);

  // write symbols
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


static void reg_sym(astencode_t* a, sym_t sym) {
  sym_t* vp = array_sortedset_assign(
    sym_t, &a->symmap, a->ma, &sym, (array_sorted_cmp_t)ptr_cmp, NULL);
  if UNLIKELY(!vp) {
    a->oom = true;
  } else if (*vp == NULL) {
    *vp = sym;
    a->symsize += strlen(sym) + 1;
  }
}


static void reg_syms(astencode_t* a, const node_t* n) {
  if (a->oom)
    return;

  #define ADDSYM(sym)  ( reg_sym(a, assertnotnull(sym)) )
  #define ADDSYMZ(sym) ( (sym) ? reg_sym(a, (sym)) : ((void)0) )

  if (node_istype(n))
    ADDSYMZ(((const type_t*)n)->tid);

  switch ((enum nodekind)n->kind) {

  case EXPR_VAR:
  case EXPR_LET:
  case EXPR_FIELD:      return ADDSYM(((local_t*)n)->name);

  case EXPR_PARAM:      return ADDSYMZ(((local_t*)n)->name);
  case EXPR_ID:         return ADDSYM(((idexpr_t*)n)->name);
  case EXPR_MEMBER:     return ADDSYM(((member_t*)n)->name);
  case EXPR_FUN:        return ADDSYMZ(((fun_t*)n)->name);
  case TYPE_STRUCT:     return ADDSYMZ(((structtype_t*)n)->name);
  case TYPE_UNRESOLVED: return ADDSYM(((unresolvedtype_t*)n)->name);
  case TYPE_ALIAS:      return ADDSYM(((aliastype_t*)n)->name);

  // no symbols
  case NODE_BAD:
  case NODE_COMMENT:
  case NODE_UNIT:
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
  case TYPE_OPTIONAL:
  case TYPE_PTR:
  case TYPE_REF:
  case TYPE_SLICE:
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


static void* nullable enc_tmpalloc(astencode_t* a, usize size) {
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


static void* nullable clone_node_shallow(astencode_t* a, const node_t* n) {
  usize nodesize = g_nodekindsizetab[n->kind];
  node_t* n2 = enc_tmpalloc(a, nodesize);
  if LIKELY(n2)
    memcpy(n2, n, nodesize);
  return n2;
}


static const node_t* pub_api_filter_unit(astencode_t* a, const unit_t* unit1) {
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


static const node_t* pub_api_filter_node(astencode_t* a, const node_t* n) {
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


static void add_ast_visitor(astencode_t* a, u32 flags, const node_t* n) {
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

  // visit expression's type
  if (node_isexpr(n) && ((expr_t*)n)->type) {
    const expr_t* expr = (expr_t*)n;
    // dlog("visit type %s %p", nodekind_name(expr->type->kind), expr->type);
    add_ast_visitor(a, flags, (node_t*)expr->type);
  }

  if (flags & AENC_PUB_API)
    n = pub_api_filter_node(a, n);

  // visit each child
  astiter_t childit = astiter_of_children(n);
  for (const node_t* cn; (cn = astiter_next(&childit));) {
    // dlog("visit child %s %p", nodekind_name(cn->kind), cn);
    add_ast_visitor(a, flags, cn);
  }
  astiter_dispose(&childit);

  // append n to nodelist
  // dlog("add nodelist[%u] <= %s %p", a->nodelist.len, nodekind_name(n->kind), n);
  if UNLIKELY(!nodearray_push(&a->nodelist, a->ma, (void*)n) && !a->oom) {
    dlog("%s: nodearray_push OOM", __FUNCTION__);
    a->oom = true;
  }

  // assign its nodelist index to nodemap
  *vp = a->nodelist.len; // +1 since 0 is initial value, checked for above
}


err_t astencode_add_ast(astencode_t* a, const node_t* n, u32 flags) {
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
  u32 srcfileid = loc_srcfileid(n->loc);
  if (srcfileid > 0) {
    if (!u32array_sortedset_add(&a->srcfileids, a->ma, srcfileid))
      return ErrNoMem;
  }

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

  // it's illegal to call astencode_add_ast twice with the same node
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


err_t astencode_init(astencode_t* a, memalloc_t ma, locmap_t* locmap) {
  memset(a, 0, sizeof(*a));
  a->ma = ma;
  a->locmap = locmap;
  err_t err = map_init(&a->nodemap, ma, 256) ? 0 : ErrNoMem;
  return err;
}


void astencode_reset(astencode_t* a, locmap_t* locmap) {
  a->nodelist.len = 0;
  a->symmap.len = 0;
  a->rootlist.len = 0;
  a->srcfileids.len = 0;
  a->locmap = locmap;
  map_clear(&a->nodemap);
}


void astencode_dispose(astencode_t* a) {
  for (u32 i = 0; i < a->tmpallocs.len; i++)
    mem_freex(a->ma, a->tmpallocs.v[i]);
  array_dispose(mem_t, (array_t*)&a->tmpallocs, a->ma);

  nodearray_dispose(&a->nodelist, a->ma);
  ptrarray_dispose(&a->symmap, a->ma);
  u32array_dispose(&a->rootlist, a->ma);
  u32array_dispose(&a->srcfileids, a->ma);
  map_dispose(&a->nodemap, a->ma);
}


//———————————————————————————————————————————————————————————————————————————————————————
// decoder

typedef struct {
  u32         version;
  u32         symcount;  // size of symtab
  u32         nodecount; // size of nodetab
  u32         rootcount; // number of root nodes at the tail of nodetab
  u32         srccount;  // size of srctab
  sym_t*      symtab;    // ID => sym_t
  node_t**    nodetab;   // ID => node_t*
  u32*        srctab;    // ID => srcfileid (document local ID => global ID)
  memalloc_t  ma;
  memalloc_t  ast_ma;
  pkg_t*      pkg;
  locmap_t*   locmap;
  const char* srcname;
  const u8*   pstart;
  err_t       err;
} astdecode_t;

#define DEC_PARAMS  astdecode_t* d, const u8* pend, const u8* p
#define DEC_ARGS    d, pend, p

#define DEC_DATA_AVAIL  ((usize)(uintptr)(pend - p))


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


static const u8* dec_nodearray(DEC_PARAMS, nodearray_t* dstp) {
  // nodearray = "*" len (SP u32x){len}
  if UNLIKELY(DEC_DATA_AVAIL < 2 || *p != '*')
    return DEC_ERROR(ErrInvalid, "expected '*N'");
  p++; // consume '*'

  // read array length
  u32 len = 0;
  p = dec_u32x(DEC_ARGS, &len);
  if UNLIKELY((usize)len > USIZE_MAX / sizeof(void*))
    return DEC_ERROR(ErrOverflow, "node array too large (%u)", len);

  // allocate memory for array
  mem_t m = mem_alloc(d->ast_ma, (usize)len * sizeof(void*));
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
      return DEC_ERROR(ErrInvalid, "invalid node ID %u", id);
    v[i] = assertnotnull(d->nodetab[i]);
  }

  if (d->err)
    return p;

  dstp->v = v;
  dstp->cap = m.size / sizeof(void*);
  dstp->len = len;

  return p;
}


static const u8* decode_field(DEC_PARAMS, void* fp, ae_field_t f) {
  // dlog("  %-12s %-9s  +%u", f.name, ae_fieldtype_str(f.type), f.offs);

  // set fp to point to the field of the node_t
  fp += f.offs;

  p = dec_byte(DEC_ARGS, ' ');

  switch ((enum ae_fieldtype)f.type) {
    case AFT_U8:        return dec_u8x(DEC_ARGS, fp);
    case AFT_U16:       return dec_u16x(DEC_ARGS, fp);
    case AFT_U32:       return dec_u32x(DEC_ARGS, fp);
    case AFT_U64:       return dec_u64x(DEC_ARGS, fp);
    case AFT_F64:       return dec_f64x(DEC_ARGS, fp);
    case AFT_LOC:       return dec_loc(DEC_ARGS, fp);
    case AFT_SYM:       return dec_symref(DEC_ARGS, fp, /*allow_null*/false); break;
    case AFT_SYMZ:      return dec_symref(DEC_ARGS, fp, /*allow_null*/true); break;
    case AFT_NODE:      return dec_noderef(DEC_ARGS, fp, /*allow_null*/false); break;
    case AFT_NODEZ:     return dec_noderef(DEC_ARGS, fp, /*allow_null*/true); break;
    case AFT_STR:       return dec_str(DEC_ARGS, fp, /*allow_null*/false); break;
    case AFT_STRZ:      return dec_str(DEC_ARGS, fp, /*allow_null*/true); break;
    case AFT_NODEARRAY: return dec_nodearray(DEC_ARGS, fp); break;
    case AFT_CUSTOM:    panic("TODO decode CUSTOM field"); break;
    case AFT_UNDEF:     UNREACHABLE; break;
  }

  return p;
}


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
  p = dec_u32x(DEC_ARGS, &d->symcount);
  p = dec_whitespace(DEC_ARGS);
  p = dec_u32x(DEC_ARGS, &d->nodecount);
  p = dec_whitespace(DEC_ARGS);
  p = dec_u32x(DEC_ARGS, &d->rootcount);
  p = dec_byte(DEC_ARGS, '\n');

  if (d->version != 1 && !d->err) {
    dlog("unsupported version: %u", d->version);
    d->err = ErrNotSupported;
  }
  if (d->rootcount > d->nodecount && !d->err) {
    dlog("invalid rootcount: %u", d->rootcount);
    d->err = ErrInvalid;
  }
  return p;
}


static const u8* decode_line(DEC_PARAMS, const char** startp, usize* lenp) {
  // decode LF-terminated line (consumes terminating LF)
  *startp = (const char*)p;
  while (p < pend && *p != '\n')
    p++;
  if UNLIKELY(p == pend) {
    *lenp = 0;
    return DEC_ERROR(ErrInvalid);
  }
  *lenp = (usize)(uintptr)(p - *(const u8**)startp);
  return p + 1;
}


static const u8* decode_srctab(DEC_PARAMS) {
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
    p = decode_line(DEC_ARGS, &linep, &linelen);

    // allocate or retrieve srcfile for pkg
    srcfile_t* srcfile = pkg_add_srcfile(d->pkg, linep, linelen);
    if UNLIKELY(!srcfile)
      return DEC_ERROR(ErrNoMem, "pkg_add_srcfile OOM");

    // intern in locmap, resulting in our "local" srcfileid
    u32 srcfileid = locmap_srcfileid(d->locmap, srcfile, d->ma);
    if UNLIKELY(srcfileid == 0)
      return DEC_ERROR(ErrNoMem, "locmap_srcfileid OOM");

    // dlog("srctab[%u] = srcfile #%u (\"%s\")", i, srcfileid, srcfile->name.p);
    d->srctab[i] = srcfileid;
  }
  return p;
}


static const u8* decode_srcinfo(DEC_PARAMS) {
  // srcinfo   = pkgpath? LF
  //             srcdir? LF
  //             srccount LF (srcfile LF){srccount}
  //             importcount LF (pkgpath LF){importcount}
  // pkgpath   = filepath
  // srcdir    = filepath
  // srccount  = u32x
  // srcfile   = filepath
  // mtime     = u64x

  const char* linep;
  usize linelen;

  // pkgpath LF
  p = decode_line(DEC_ARGS, &linep, &linelen);
  dlog(">> pkgpath = '%.*s'", (int)linelen, linep);
  str_free(d->pkg->path);
  d->pkg->path = str_makelen(linep, linelen);

  // srcdir LF
  p = decode_line(DEC_ARGS, &linep, &linelen);
  dlog(">> srcdir = '%.*s'", (int)linelen, linep);
  str_free(d->pkg->dir);
  d->pkg->dir = str_makelen(linep, linelen);

  // srccount LF (srcfile LF){srccount}
  p = dec_u32x(DEC_ARGS, &d->srccount);
  p = dec_byte(DEC_ARGS, '\n');
  p = decode_srctab(DEC_ARGS);

  // importcount LF (pkgpath LF){importcount}
  u32 importcount;
  p = dec_u32x(DEC_ARGS, &importcount);
  p = dec_byte(DEC_ARGS, '\n');
  for (u32 i = 0; i < importcount; i++) {
    p = decode_line(DEC_ARGS, &linep, &linelen);
    panic("TODO import %.*s", (int)linelen, linep);
    //
    // TODO: imports
    //
    // This is a little tricky: we can't simply popluate d->pkg->imports
    // since that would require us to resolve packages up front, here and now,
    // which would be slow.
    //
    // Another aspect of this is that the AST we're decoding is referencing
    // nodes of the imported packages, which we need to resolve.
    //
    // See import_resolve_pkg
    //
  }

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
  const ae_field_t* fieldtab = g_fieldtab[kind];

  // intercept universal types, singletons compared by address,
  // by looking for nodes that are represented solely by type_t
  if (fieldtab == g_fieldsof_type_t)
    return decode_universal_node(DEC_ARGS, node_id, kind);

  // decode standard node, starting by allocating memory for the node struct
  usize nodesize = g_nodekindsizetab[kind];
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
  for (u8 i = 0; i < g_fieldlentab[n->kind]; i++) {
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


static bool dec_tmptabs_alloc(astdecode_t* d) {
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
  if (!p) {
    d->err = ErrNoMem;
    return false;
  }

  d->nodetab = p;
  d->symtab = p + nbyte_nodetab;

  return true;
}


static void dec_tmptabs_free(astdecode_t* d) {
  if (!d->nodetab)
    return;
  usize nbyte = ((usize)d->nodecount * sizeof(*d->nodetab))
              + ((usize)d->symcount * sizeof(*d->symtab))
              + ((usize)d->srccount * sizeof(*d->srctab));
  nbyte = ALIGN2(nbyte, sizeof(void*));
  mem_freex(d->ma, MEM(d->nodetab, nbyte));
}


err_t astdecode(
  memalloc_t ma,
  memalloc_t ast_ma,
  pkg_t* pkg,
  locmap_t* locmap,
  const char* srcname, const u8* src, usize srclen,
  node_t*** resultv, u32* resultc
) {
  if (!IS_ALIGN2((uintptr)src, 4)) {
    dlog("src %p not 4-byte aligned", src);
    return ErrInvalid;
  }

  astdecode_t dec_ = {
    .pstart = src,
    .srcname = srcname,
    .ast_ma = ast_ma,
    .ma = ma,
    .pkg = pkg,
    .locmap = locmap,
  };

  node_t** roots = NULL;

  // DEC_ARGS
  astdecode_t* d = &dec_;
  const u8* p = src;
  const u8* pend = p + srclen;

  // decode header
  p = decode_header(DEC_ARGS);
  if (d->err)
    return d->err;
  // dlog("header: symcount %u, nodecount %u, rootcount %u",
  //   d->symcount, d->nodecount, d->rootcount);

  // allocate memory for temporary tables
  if (!dec_tmptabs_alloc(d))
    return d->err;

  // decode srcinfo
  p = decode_srcinfo(DEC_ARGS);
  if (d->err)
    goto error;

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
  roots = mem_alloc(ast_ma, (usize)d->rootcount * sizeof(void*)).p;
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
    mem_freex(ast_ma, MEM(roots, (usize)d->rootcount * sizeof(void*)));
  *resultv = NULL;
  *resultc = 0;
end:
  if (d->srctab)
    mem_freex(d->ma, MEM(d->srctab, (usize)d->srccount * sizeof(*d->srctab)));
  dec_tmptabs_free(d);
  return d->err;
}
