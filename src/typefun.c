// SPDX-License-Identifier: Apache-2.0
#include "colib.h"
#include "compiler.h"
#include "hash.h"
#include "tmpbuf.h"


typedef struct {
  type_t* recvt;
  sym_t   name;
  fun_t*  fn;
} tfunent_t;


static usize tfunent_hash(usize seed, const void* entp) {
  const tfunent_t* ent = entp;

  u64 hash = wyhash64(seed, (uintptr)ent->name);

  assertnotnull(ent->recvt->_typeid);
  hash = typeid_hash(hash, ent->recvt->_typeid);

  {
    buf_t* tmpbuf = tmpbuf_get(0);
    buf_appendrepr(tmpbuf, ent->recvt->_typeid, typeid_len(ent->recvt->_typeid));
    dlog("tfunent_hash (\"%.*s\" \"%s\") => 0x%llx",
      (int)tmpbuf->len, tmpbuf->chars, ent->name, hash);
  }

  return hash;
}


static bool tfunent_eq(const void* ent1, const void* ent2) {
  const tfunent_t* a = ent1;
  const tfunent_t* b = ent2;
  assertnotnull(a->recvt->_typeid);
  assertnotnull(b->recvt->_typeid);
  {
    buf_t* a_typeid = tmpbuf_get(0);
    buf_t* b_typeid = tmpbuf_get(1);
    buf_appendrepr(a_typeid, a->recvt->_typeid, typeid_len(a->recvt->_typeid));
    buf_appendrepr(b_typeid, b->recvt->_typeid, typeid_len(b->recvt->_typeid));
    dlog("tfunent_eq (%.*s %s) <> (%.*s %s)",
      (int)a_typeid->len, a_typeid->chars, a->name,
      (int)b_typeid->len, b_typeid->chars, b->name);
  }
  return (a->recvt->_typeid == b->recvt->_typeid && a->name == b->name);
}


err_t typefuntab_init(typefuntab_t* tfuns, memalloc_t ma) {
  err_t err;
  if (( err = rwmutex_init(&tfuns->mu) ))
    return err;
  if (( err = hashtable_init(&tfuns->ht, ma, sizeof(tfunent_t), 16) )) {
    rwmutex_dispose(&tfuns->mu);
    return err;
  }
  return 0;
}


void typefuntab_dispose(typefuntab_t* tfuns) {
  hashtable_dispose(&tfuns->ht, sizeof(tfunent_t));
}


fun_t* nullable typefuntab_add(typefuntab_t* tfuns, type_t* t, sym_t name, fun_t* fn) {
  bool added;
  typeid_intern(t);
  tfunent_t key = {
    .recvt = t,
    .name = name,
    .fn = fn,
  };
  rwmutex_lock(&tfuns->mu);
  tfunent_t* ent = hashtable_assign(
    &tfuns->ht, tfunent_hash, tfunent_eq, sizeof(tfunent_t),
    &key, &added);
  rwmutex_unlock(&tfuns->mu);
  if (!ent)
    return NULL;
  if (!added)
    fn = ent->fn;
  // dlog("[%s] (%s#%p %s).%s %s %s", __FUNCTION__,
  //   nodekind_name(t->kind), t, fmtnode(0, t), name,
  //   (added ? "add" : "intern"), fmtnode(1, fn));
  return fn;
}


fun_t* nullable typefuntab_lookup(typefuntab_t* tfuns, type_t* t, sym_t name) {
  // look for type function, considering alias types.
  //   1. unwrap optional, ref and ptr type so that e.g. &MyMyT becomes MyMyT.
  //   2. Lookup MyT.name in tfuns (alias of T), if found return the function.
  //   3. if MyT is an alias, unwrap MyT => T, repeat steps 1–3
  //   4. not found; return NULL
  //
  tfunent_t* ent;
  typeid_intern(t);
  tfunent_t key = {
    .recvt = t,
    .name = name,
  };
  rwmutex_rlock(&tfuns->mu);
  for (;;) {
    key.recvt = type_unwrap_ptr(key.recvt); // e.g. "&T" => "T"
    ent = hashtable_lookup(
      &tfuns->ht, tfunent_hash, tfunent_eq, sizeof(tfunent_t),
      &key);
    // end now if we found a function, or if recvt is not an alias
    if (ent || key.recvt->kind != TYPE_ALIAS)
      break;
    // recvt is an alias; look for a type function defined for its underlying type
    key.recvt = assertnotnull(((aliastype_t*)key.recvt)->elem);
  }
  rwmutex_runlock(&tfuns->mu);
  // dlog("[%s] (%s#%p %s).%s => %s", __FUNCTION__,
  //   nodekind_name(t->kind), t, fmtnode(0, t), name,
  //   ent ? fmtnode(1, ent->fn) : "(not found)");
  return ent ? ent->fn : NULL;
}
