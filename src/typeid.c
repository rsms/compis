// SPDX-License-Identifier: Apache-2.0
#include "colib.h"
#include "ast_field.h"
#include "compiler.h"
#include "hashtable.h"
#include "hash.h"
#include "leb128.h"


#define TYPEID_TRACE
#ifdef TYPEID_TRACE
  #define trace(fmt, args...) _dlog(6, "typeid", __FILE__, __LINE__, fmt, ##args)
#else
  #define trace(fmt, args...) ((void)0)
#endif


#define TYPEID_TAG_ARRAY '['
#define TYPEID_TAG_SYM   '#'
#define TYPEID_TAG_STR   '"'
#define TYPEID_TAG_REF   '&'
#define TYPEID_TAG_TID   '$'


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
  return wyhash(typeid, typeid_len(typeid), seed, secret);
}


static usize _typeid_hash(usize seed, const void* typeidp) {
  typeid_t typeid = *(typeid_t*)typeidp;
  return typeid_hash(seed, typeid);
}


static bool typeid_eq(const void* ap, const void* bp) {
  typeid_t a = *(typeid_t*)ap, b = *(typeid_t*)bp;
  u32 az = typeid_len(a), bz = typeid_len(b);
  return az == bz && memcmp(a, b, az) == 0;
}


void typeid_init(memalloc_t ma) {
  typeid_ma = ma;
  safecheckx(rwmutex_init(&typeid_mu) == 0);
  UNUSED err_t err = hashtable_init(&typeid_ht, ma, sizeof(typeid_t), 256);
  safecheckf(err == 0, "hashtable_init: %s", err_str(err));
}


static typeid_t typeid_map_intern(typeid_t typeid) {
  #ifdef TYPEID_TRACE
  {
    buf_t tmpbuf = buf_make(typeid_ma);
    buf_appendrepr(&tmpbuf, typeid, typeid_len(typeid));
    trace("[typeid_map_intern] typeid(%u)='%.*s' (hash=0x%lx)",
      typeid_len(typeid), (int)tmpbuf.len, tmpbuf.chars,
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
      buf_t tmpbuf = buf_make(typeid_ma);
      buf_appendrepr(&tmpbuf, typeid, typeid_len(typeid));
      trace("use existing typeid %p (%u) '%.*s'",
        typeid, typeid_len(typeid), (int)tmpbuf.len, tmpbuf.chars);
      buf_dispose(&tmpbuf);
    }
    #endif

    return *ent;
  }

  // not found; assign under full write lock
  rwmutex_lock(&typeid_mu);

  bool did_insert;
  ent = hashtable_assign(
    &typeid_ht, _typeid_hash, typeid_eq, sizeof(typeid_t), &typeid, &did_insert);
  if UNLIKELY(!ent)
    goto oom;

  if (did_insert) {
    usize nbyte = (usize)typeid_len(typeid) + 4;
    u8* bytes = mem_alloc(typeid_ma, nbyte).p;
    if UNLIKELY(!bytes)
      goto oom;
    memcpy(bytes, typeid - 4, nbyte);
    *ent = bytes + 4;
    typeid = *ent;
  }

  #ifdef TYPEID_TRACE
  {
    buf_t tmpbuf = buf_make(typeid_ma);
    buf_appendrepr(&tmpbuf, typeid, typeid_len(typeid));
    trace("[typeid_map_intern] %s typeid %p (%u) '%.*s'",
      did_insert ? "add" : "use existing",
      typeid, typeid_len(typeid), (int)tmpbuf.len, tmpbuf.chars);
    buf_dispose(&tmpbuf);
  }
  #endif

  rwmutex_unlock(&typeid_mu);

  return typeid;
oom:
  panic("out of memory");
  return NULL;
}


#define PARAMS  buf_t* buf, bool intern, nodearray_t* seenstack, int ind
#define ARGS    buf, intern, seenstack, ind


static void typeid_fmt_node1(PARAMS, node_t* n);


static typeid_t typeid_make(PARAMS, type_t* t) {
  usize buf_offs = buf->len;
  safecheckxf(buf_reserve(buf, 256), "out of memory");

  // reserve 4 bytes (4-byte aligned) for length prefix
  usize start_offs = ALIGN2(buf_offs, 4);
  buf->len += 4 + (start_offs - buf_offs);

  typeid_fmt_node1(ARGS, (node_t*)t);

  safecheckf(!buf->oom, "out of memory");

  // set length prefix
  typeid_t typeid = &buf->bytes[start_offs];
  assertf(IS_ALIGN2((uintptr)typeid, 4), "%p", typeid);
  safecheck(buf->len - start_offs <= (usize)U32_MAX);
  *(u32*)typeid = (u32)(buf->len - start_offs);

  typeid = typeid_map_intern(typeid + 4);
  buf->len = buf_offs;

  if (intern)
    t->_typeid = typeid;
  return typeid;
}


static void typeid_fmt_type(PARAMS, type_t* t) {
  typeid_t typeid;
  if (t->_typeid) {
    typeid = t->_typeid;
  } else {
    typeid = typeid_make(ARGS, t);
  }
  buf_push(buf, TYPEID_TAG_TID);
  buf_append(buf, typeid, typeid_len(typeid));
}


static void typeid_fmt_cstr(buf_t* buf, u8 tag, const char* cstr) {
  usize len = strlen(cstr);
  buf_push(buf, tag);
  buf_print_leb128_u64(buf, len);
  buf_append(buf, cstr, len);
}


static void typeid_fmt_node(PARAMS, node_t* n) {
  if (node_istype(n))
    return typeid_fmt_type(ARGS, (type_t*)n);
  typeid_fmt_node1(ARGS, n);
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

  trace("%*s -> typeid_fmt %s %p", ind, "", nodekind_name(n->kind), n);
  ind += 2;

  // kind
  assert(n->kind < NODEKIND_COUNT);
  buf_append(buf, &g_ast_kindtagtab[n->kind], 4);

  // flags
  buf_print_leb128_u64(buf, n->flags & NODEFLAGS_TYPEID_MASK);

  // expr.type
  if (node_isexpr(n) && ((expr_t*)n)->type)
    typeid_fmt_type(ARGS, ((expr_t*)n)->type);

  const ast_field_t* fieldtab = g_ast_fieldtab[n->kind];
  u8 fieldlen = g_ast_fieldlentab[n->kind];
  u64 u64val;

  for (u8 fieldidx = 0; fieldidx < fieldlen; fieldidx++) {
    ast_field_t f = fieldtab[fieldidx];
    void* fp = (void*)n + f.offs;

    if (f.isid == 0)
      continue;

    trace("%*s : %s %s (+%u)", ind, "", f.name, ast_fieldtype_str(f.type), f.offs);

  switch_again:
    switch ((enum ast_fieldtype)f.type) {

    case AST_FIELD_NODEZ:
    case AST_FIELD_SYMZ:
    case AST_FIELD_STRZ:
      if (*(void**)fp == NULL)
        break;
      f.type--;
      goto switch_again;

    case AST_FIELD_NODE:
      typeid_fmt_node(ARGS, *(node_t**)fp);
      break;

    case AST_FIELD_NODEARRAY: {
      nodearray_t* na = fp;
      buf_push(buf, TYPEID_TAG_ARRAY);
      buf_print_leb128_u64(buf, na->len);
      for (u32 i = 0; i < na->len; i++)
        typeid_fmt_node(ARGS, na->v[i]);
      break;
    }

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
  trace("%*s <— typeid_fmt %s %p", ind, "", nodekind_name(n->kind), n);
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
