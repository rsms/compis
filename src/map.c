// SPDX-License-Identifier: Apache-2.0
#include "colib.h"
#include "map.h"
#include "abuf.h"
#include "hash.h"

#define DELMARK ((const char*)1)

// lf is a bit shift magnitude that does fast integer division
// i.e. cap-(cap>>lf) == (u32)((double)cap*0.75)
// i.e. grow when 1=50% 2=75% 3=88% 4=94% full
#define LOAD_FACTOR     2
#define LOAD_FACTOR_MUL 0.25 // with LOAD_FACTOR 1=0.5 2=0.25 3=0.125 4=0.0625


static usize keyhash(const void *key, usize keysize, u64 seed) {
  // static const u8 secret8[4];
  // static const u64 secret[4] = {
  //   (u64)(uintptr)&secret8[0], (u64)(uintptr)&secret8[1],
  //   (u64)(uintptr)&secret8[2], (u64)(uintptr)&secret8[3] };
  static const u64 secret[4] = {
    0xdb1949b0945c5256llu,
    0x4f85e17c1e7ee8allu,
    0x24ac847a1c0d4bf7llu,
    0xd2952ed7e9fbaf43llu };
  return wyhash(key, keysize, seed, secret);
}


static usize ptrhash(const void *key, u64 seed) {
  return wyhash64((u64)(uintptr)key, seed);
}


inline static bool keyeq(const mapent_t* ent, const void* key, usize keysize) {
  return ent->keysize == keysize && memcmp(ent->key, key, keysize) == 0;
}


static u32 idealcap(u32 lenhint) {
  // lenhint + 1: must always have one free slot
  double f = (double)(lenhint + 1u)*LOAD_FACTOR_MUL + 0.5;
  if UNLIKELY(f > (double)U32_MAX)
    return U32_MAX;
  return CEIL_POW2(lenhint + 1u + (u32)f);
}


bool map_init(map_t* m, memalloc_t ma, u32 lenhint) {
  assert(lenhint > 0);
  u32 cap = idealcap(lenhint);
  m->len = 0;
  m->cap = cap;
  m->seed = fastrand();
  m->entries = mem_alloctv(ma, mapent_t, cap);
  return m->entries != NULL;
}


void map_clear(map_t* m) {
  m->len = 0;
  memset(m->entries, 0, m->cap * sizeof(mapent_t));
}


static void map_relocate(u64 seed, mapent_t* entries, u32 cap, mapent_t* ent) {
  usize index = keyhash(ent->key, ent->keysize, seed) & (cap - 1);
  while (entries[index].key) {
    if (keyeq(&entries[index], ent->key, ent->keysize))
      break;
    if (++index == cap)
      index = 0;
  }
  entries[index] = *ent;
}


static bool map_grow1(map_t* m, memalloc_t ma, u32 newcap) {
  //dlog("grow cap %u => %u (%zu B)", m->cap, newcap, (usize)newcap*sizeof(mapent_t));
  mapent_t* newentries = mem_alloctv(ma, mapent_t, newcap);
  if UNLIKELY(!newentries)
    return false;
  m->seed = fastrand();
  for (u32 i = 0; i < m->cap; i++) {
    mapent_t ent = m->entries[i];
    if (ent.key && ent.key != DELMARK)
      map_relocate(m->seed, newentries, newcap, &ent);
  }
  mem_freetv(ma, m->entries, m->cap);
  m->entries = newentries;
  m->cap = newcap;
  return true;
}


static bool map_grow(map_t* m, memalloc_t ma) {
  u32 newcap;
  if (check_mul_overflow(m->cap, (u32)2u, &newcap))
    return false;
  return map_grow1(m, ma, newcap);
}


bool map_reserve(map_t* m, memalloc_t ma, u32 addlen) {
  u32 newlen;
  if (check_add_overflow(m->len, addlen, &newlen))
    return false;
  u32 newcap = idealcap(newlen);
  if (newcap <= m->cap)
    return true;
  return map_grow1(m, ma, newcap);
}


mapent_t* nullable map_assign_ent(
  map_t* m, memalloc_t ma, const void* key, usize keysize)
{
  u32 growlen = m->cap - (m->cap >> LOAD_FACTOR);
  if (UNLIKELY(m->len >= growlen) && !map_grow(m, ma))
    return NULL;
  usize index = keyhash(key, keysize, m->seed) & (m->cap - 1);
  while (m->entries[index].key) {
    if (keyeq(&m->entries[index], key, keysize))
      return &m->entries[index];
    if (m->entries[index].key == DELMARK) // recycle deleted slot
      break;
    if (++index == m->cap)
      index = 0;
  }
  m->len++;
  m->entries[index].key = key;
  m->entries[index].keysize = keysize;
  return &m->entries[index];
}


void** nullable map_assign(map_t* m, memalloc_t ma, const void* key, usize keysize) {
  mapent_t* ent = map_assign_ent(m, ma, key, keysize);
  return ent ? &ent->value : NULL;
}


void** nullable map_lookup(const map_t* m, const void* key, usize keysize) {
  usize index = keyhash(key, keysize, m->seed) & (m->cap - 1);
  while (m->entries[index].key) {
    if (keyeq(&m->entries[index], key, keysize))
      return &m->entries[index].value;
    if (++index == m->cap)
      index = 0;
  }
  if (m->parent)
    return map_lookup(m->parent, key, keysize);
  return NULL;
}


static void map_del_ent1(map_t* m, mapent_t* ent) {
  m->len--;
  ent->key = DELMARK;
  ent->keysize = 0;
}


void map_del_ent(map_t* m, mapent_t* ent) {
  #if DEBUG
  {
    // can't use lookup since key might be ptr or bytes; we don't know
    bool ok = false;
    for (const mapent_t* e = map_it(m); map_itnext(m, &e); ) {
      if (e == ent) {
        ok = true;
        break;
      }
    }
    assertf(ok, "ent not in map");
  }
  #endif

  if (m->len == 1)
    return map_clear(m); // clear all DELMARK entries
  map_del_ent1(m, ent);
}


static bool map_del1(map_t* m, void* nullable vp) {
  if UNLIKELY(vp == NULL)
    return false;
  if (m->len == 1) {
    map_clear(m); // clear all DELMARK entries
    return true;
  }
  mapent_t* ent = vp - offsetof(mapent_t,value);
  map_del_ent1(m, ent);
  return true;
}


bool map_del(map_t* m, const void* key, usize keysize) {
  void* vp = map_lookup(m, key, keysize);
  return map_del1(m, vp);
}


void** nullable map_assign_ptr(map_t* m, memalloc_t ma, const void* key) {
  u32 growlen = m->cap - (m->cap >> LOAD_FACTOR);
  if (UNLIKELY(m->len >= growlen) && !map_grow(m, ma))
    return NULL;
  usize index = ptrhash(key, m->seed) & (m->cap - 1);
  while (m->entries[index].key) {
    if (m->entries[index].key == key)
      return &m->entries[index].value;
    if (m->entries[index].key == DELMARK) // recycle deleted slot
      break;
    if (++index == m->cap)
      index = 0;
  }
  m->len++;
  m->entries[index].key = key;
  m->entries[index].keysize = sizeof(void*);
  return &m->entries[index].value;
}


void** nullable map_lookup_ptr(const map_t* m, const void* key) {
  usize index = ptrhash(key, m->seed) & (m->cap - 1);
  while (m->entries[index].key) {
    if (m->entries[index].key == key)
      return &m->entries[index].value;
    if (++index == m->cap)
      index = 0;
  }
  if (m->parent)
    return map_lookup_ptr(m->parent, key);
  return NULL;
}


bool map_del_ptr(map_t* m, const void* key) {
  void* vp = map_lookup_ptr(m, key);
  return map_del1(m, vp);
}


bool map_itnext(const map_t* m, const mapent_t** ep) {
  for (const mapent_t* e = (*ep)+1, *end = m->entries + m->cap; e != end; e++) {
    if (e->key && e->key != DELMARK) {
      *ep = e;
      return true;
    }
  }
  return false;
}


bool map_update_replace_ptr(map_t* m, memalloc_t ma, const map_t* src) {
  if UNLIKELY(!map_reserve(m, ma, src->len))
    return false;
  for (const mapent_t* e = map_it(src); map_itnext(src, &e); ) {
    void** vp = assertnotnull(map_assign_ptr(m, ma, e->key));
    if (!vp)
      return false;
    *vp = e->value;
  }
  return true;
}
