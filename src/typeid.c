// SPDX-License-Identifier: Apache-2.0
#include "colib.h"
#include "ast_field.h"
#include "compiler.h"
#include "hashtable.h"
#include "hash.h"
#include "leb128.h"


// #define TYPEID_TRACE
#ifdef TYPEID_TRACE
  #define trace(fmt, args...) _dlog(6, "typeid", __FILE__, __LINE__, fmt, ##args)
#else
  #define trace(fmt, args...) ((void)0)
#endif


#define TYPEID_TAG_ARRAY '['
#define TYPEID_TAG_SYM   '#'
#define TYPEID_TAG_STR   '"'
#define TYPEID_TAG_REF   '&'
// #define TYPEID_TAG_TID   '$'


static memalloc_t  typeid_ma;
static rwmutex_t   typeid_mu;
static hashtable_t typeid_ht;


// Conversion of f64 <-> u64
// Currently assumes f64 is represented as IEEE 754 binary64
inline static u64 f64_to_u64(f64 v) { return *(u64*)&v; }
//inline static f64 u64_to_f64(u64 v) { return *(f64*)&v; }


usize typeid_hash(usize seed, typeid_t typeid) {
  static const u64 secret[4] = {
    0xdb1949b0945c5256llu,
    0x4f85e17c1e7ee8allu,
    0x24ac847a1c0d4bf7llu,
    0xd2952ed7e9fbaf43llu };
  return wyhash(typeid->bytes, typeid->len, seed, secret);
}


static usize _typeid_hash(usize seed, const void* typeidp) {
  typeid_t typeid = *(typeid_t*)typeidp;
  return typeid_hash(seed, typeid);
}


static bool typeid_eq(const void* ap, const void* bp) {
  typeid_t a = *(typeid_t*)ap;
  typeid_t b = *(typeid_t*)bp;
  return a->len == b->len && memcmp(a->bytes, b->bytes, a->len) == 0;
}


void typeid_init(memalloc_t ma) {
  typeid_ma = ma;
  safecheckx(rwmutex_init(&typeid_mu) == 0);
  UNUSED err_t err = hashtable_init(&typeid_ht, ma, sizeof(typeid_t), 256);
  safecheckf(err == 0, "hashtable_init: %s", err_str(err));
}


typeid_t typeid_intern_typeid(typeid_t typeid) {

  rwmutex_lock(&typeid_mu);

  bool did_insert;
  typeid_t* ent = hashtable_assign(
    &typeid_ht, _typeid_hash, typeid_eq, sizeof(typeid_t), &typeid, &did_insert);
  if UNLIKELY(!ent)
    goto oom;

  if (did_insert) {
    usize nbyte = sizeof(typeid_data_t) + typeid->len;
    typeid_data_t* typeid2 = mem_alloc(typeid_ma, nbyte).p;
    if UNLIKELY(!typeid2)
      goto oom;
    memcpy(typeid2, typeid, nbyte);
    *ent = typeid2;
  }

  #ifdef TYPEID_TRACE
  {
    typeid = *ent;
    buf_t tmpbuf = buf_make(typeid_ma);
    buf_appendrepr(&tmpbuf, typeid->bytes, typeid->len);
    trace("[%s] %s typeid %p (%u) '%.*s'", __FUNCTION__,
      did_insert ? "add" : "use existing",
      typeid, typeid->len, (int)tmpbuf.len, tmpbuf.chars);
    buf_dispose(&tmpbuf);
  }
  #endif

  rwmutex_unlock(&typeid_mu);

  return *ent;
oom:
  panic("out of memory");
  UNREACHABLE;
}


static typeid_t typeid_map_intern(typeid_t typeid) {
  #ifdef TYPEID_TRACE
  {
    buf_t tmpbuf = buf_make(typeid_ma);
    buf_appendrepr(&tmpbuf, typeid->bytes, typeid->len);
    trace("[%s] typeid(%u)='%.*s' (hash=0x%lx)", __FUNCTION__,
      typeid->len, (int)tmpbuf.len, tmpbuf.chars,
      typeid_hash(typeid_ht.seed, typeid));
    buf_dispose(&tmpbuf);
  }
  #endif

  // lookup under read-only lock
  rwmutex_rlock(&typeid_mu);
  typeid_t* ent = hashtable_lookup(
    &typeid_ht, _typeid_hash, typeid_eq, sizeof(typeid_t), &typeid);
  rwmutex_runlock(&typeid_mu);

  if (ent) {
    #ifdef TYPEID_TRACE
    {
      typeid = *ent;
      buf_t tmpbuf = buf_make(typeid_ma);
      buf_appendrepr(&tmpbuf, typeid->bytes, typeid->len);
      trace("use existing typeid %p (%u) '%.*s'",
        typeid, typeid->len, (int)tmpbuf.len, tmpbuf.chars);
      buf_dispose(&tmpbuf);
    }
    #endif

    return *ent;
  }

  // not found; assign under full write lock
  return typeid_intern_typeid(typeid);
}


#define PARAMS  buf_t* buf, bool intern, nodearray_t* seenstack, int ind
#define ARGS    buf, intern, seenstack, ind


static void typeid_fmt_node1(PARAMS, node_t* n);


static typeid_t typeid_make(PARAMS, type_t* t) {
  safecheckxf(buf_reserve(buf, 256), "out of memory");

  // reserve 4 bytes (4-byte aligned) for length prefix
  usize buf_offs = buf->len;
  usize start_offs = ALIGN2(buf_offs, 4);
  // buf->len += 4 + (start_offs - buf_offs);
  buf->len = start_offs + 4;

  typeid_fmt_node1(ARGS, (node_t*)t);

  safecheckf(!buf->oom, "out of memory");

  typeid_data_t* typeid = (typeid_data_t*)&buf->bytes[start_offs];

  safecheck(buf->len - (start_offs + 4) <= (usize)U32_MAX);
  typeid->len = (u32)(buf->len - (start_offs + 4));

  typeid = (typeid_data_t*)typeid_map_intern(typeid);

  // reset buffer
  buf->len = buf_offs;

  if (intern)
    t->_typeid = typeid;
  return typeid;
}


static void typeid_fmt_type(PARAMS, type_t* t) {
  typeid_t typeid;
  if (t->_typeid) {
    #ifdef TYPEID_TRACE
    {
      buf_t tmpbuf = buf_make(typeid_ma);
      buf_appendrepr(&tmpbuf, t->_typeid->bytes, t->_typeid->len);
      trace("%*s   append existing typeid %p (%u) '%.*s'",
        ind, "", t->_typeid, t->_typeid->len, (int)tmpbuf.len, tmpbuf.chars);
      buf_dispose(&tmpbuf);
    }
    #endif
    typeid = t->_typeid;
  } else {
    typeid = typeid_make(ARGS, t); // note: sets t->_typeid if arg intern==true
  }
  //buf_push(buf, TYPEID_TAG_TID);
  buf_append(buf, typeid->bytes, typeid->len);
}


static void typeid_fmt_cstr(buf_t* buf, u8 tag, const char* cstr) {
  usize len = strlen(cstr);
  buf_push(buf, tag);
  buf_print_leb128_u64(buf, len);
  buf_append(buf, cstr, len);
}


static void typeid_fmt_node(PARAMS, node_t* n) {
  if (node_istype(n)) {
    typeid_fmt_type(ARGS, (type_t*)n);
  } else {
    typeid_fmt_node1(ARGS, n);
  }
}


static void typeid_fmt_nodearray(PARAMS, nodearray_t* na) {
  buf_push(buf, TYPEID_TAG_ARRAY);
  buf_print_leb128_u64(buf, na->len);
  for (u32 i = 0; i < na->len; i++)
    typeid_fmt_node(ARGS, na->v[i]);
}


static void typeid_fmt_node1(PARAMS, node_t* n) {
  // break cycles
  for (u32 i = seenstack->len; i > 0;) {
    if (seenstack->v[--i] == n) {
      buf_push(buf, TYPEID_TAG_REF);
      buf_print_leb128_u64(buf, i);
      return;
    }
  }
  if UNLIKELY(!nodearray_push(seenstack, buf->ma, n)) {
    dlog("OOM");
    buf->oom = true;
    return;
  }

  trace("%*s -> typeid_fmt %s n=%p buf.len=%zu",
    ind, "", nodekind_name(n->kind), n, buf->len);
  ind += 2;

  // kind
  assert(n->kind < NODEKIND_COUNT);
  buf_append(buf, &g_ast_kindtagtab[n->kind], 4);

  // flags
  buf_print_leb128_u64(buf, n->flags & NODEFLAGS_TYPEID_MASK);

  // startoffs: skip field before this offset (they are handled specially)
  usize startoffs = sizeof(node_t);
  if (node_istype(n)) {
    if (node_isusertype(n)) {
      startoffs = sizeof(usertype_t);
      usertype_t* t = (usertype_t*)n;
      // has template parameters or args?
      if (n->flags & (NF_TEMPLATE | NF_TEMPLATEI))
        typeid_fmt_nodearray(ARGS, &t->templateparams);
    } else {
      // type_t
      //
      // Note: we don't encode size or align fields since these are always the
      // same for the specific type.
      // E.g. "&[int]" is unconditionally a slice type with an "int" element;
      // it is distinct from "&[i32]", which is distinct from "&[i64]".
      //
      // Note: we ignore type_t.typeid here since typeid_fmt_node checks for this
      // before calling typeid_fmt_node1.
      //
      startoffs = sizeof(type_t);
    }
  }

  const ast_field_t* fieldtab = g_ast_fieldtab[n->kind];
  u8 fieldlen = g_ast_fieldlentab[n->kind];
  u64 u64val;

  u8 fieldidx = 0;
  for (; fieldidx < fieldlen; fieldidx++) {
    if (fieldtab[fieldidx].offs >= startoffs)
      break;
  }

  for (; fieldidx < fieldlen; fieldidx++) {
    ast_field_t f = fieldtab[fieldidx];
    if (f.isid == 0) // skip fields which are not part of an AST node's identity
      continue;
    void* fp = (void*)n + f.offs;

    trace("%*s : %s %s (+%u)", ind, "", f.name, ast_fieldtype_str(f.type), f.offs);

  switch_again:
    switch ((enum ast_fieldtype)f.type) {

    case AST_FIELD_NODEZ:
    case AST_FIELD_SYMZ:
    case AST_FIELD_STRZ:
      if (*(void**)fp == NULL)
        break;
      f.type--; // non ...Z type
      goto switch_again;

    case AST_FIELD_NODE:
      typeid_fmt_node(ARGS, *(node_t**)fp);
      break;

    case AST_FIELD_NODEARRAY:
      typeid_fmt_nodearray(ARGS, (nodearray_t*)fp);
      break;

    case AST_FIELD_U8:  u64val = (u64)*(u8*)fp; goto write_u64;
    case AST_FIELD_U16: u64val = (u64)*(u16*)fp; goto write_u64;
    case AST_FIELD_U32: u64val = (u64)*(u32*)fp; goto write_u64;
    case AST_FIELD_U64: u64val = *(u64*)fp; goto write_u64;
    case AST_FIELD_F64: u64val = f64_to_u64(*(f64*)fp); goto write_u64;

    case AST_FIELD_LOC:
      // locations are never part of type identity
      break;

    case AST_FIELD_SYM:
      typeid_fmt_cstr(buf, TYPEID_TAG_SYM, *(sym_t*)fp);
      break;

    case AST_FIELD_STR:
      typeid_fmt_cstr(buf, TYPEID_TAG_STR, *(sym_t*)fp);
      break;

    case AST_FIELD_UNDEF:
      UNREACHABLE;
      break;
    } // switch
    continue;
  write_u64:
    buf_print_leb128_u64(buf, u64val);
  }

  seenstack->len--; // pop

  ind -= 2;
  trace("%*s <— typeid_fmt %s n=%p buf.len=%zu",
    ind, "", nodekind_name(n->kind), n, buf->len);
}


typeid_t _typeid(type_t* t, bool intern) {
  memalloc_t ma = memalloc_ctx();
  buf_t buf = buf_make(ma);
  nodearray_t seenstack = {0};

  typeid_t typeid = typeid_make(&buf, intern, &seenstack, 0, t);

  nodearray_dispose(&seenstack, ma);
  buf_dispose(&buf);

  return typeid;
}
