// SPDX-License-Identifier: Apache-2.0
#include "colib.h"
#include "map.h"
#include "abuf.h"

#define DELMARK ((const char*)1)

// lf is a bit shift magnitude that does fast integer division
// i.e. cap-(cap>>lf) == (u32)((double)cap*0.75)
// i.e. grow when 1=50% 2=75% 3=88% 4=94% full
#define LOAD_FACTOR     2
#define LOAD_FACTOR_MUL 0.25 // with LOAD_FACTOR 1=0.5 2=0.25 3=0.125 4=0.0625

static u64 fastrand_state = 1;

// ———————— begin wyhash —————————
// wyhash https://github.com/wangyi-fudan/wyhash (public domain, "unlicense")
#if defined(_MSC_VER) && defined(_M_X64)
  #include <intrin.h>
  #pragma intrinsic(_umul128)
#endif
#if !CO_LITTLE_ENDIAN
  #error "big endian impl not included"
#endif
#if __LONG_MAX__ <= 0x7fffffff
  #define WYHASH_32BIT_MUM 1
#else
  #define WYHASH_32BIT_MUM 0
#endif
UNUSED static inline u64 _wyrot(u64 x) { return (x>>32)|(x<<32); }
static inline void _wymum(u64 *A, u64 *B){
#if(WYHASH_32BIT_MUM)
  u64 hh=(*A>>32)*(*B>>32), hl=(*A>>32)*(u32)*B, lh=(u32)*A*(*B>>32), ll=(u64)(u32)*A*(u32)*B;
  #if(WYHASH_CONDOM>1)
  *A^=_wyrot(hl)^hh; *B^=_wyrot(lh)^ll;
  #else
  *A=_wyrot(hl)^hh; *B=_wyrot(lh)^ll;
  #endif
#elif defined(__SIZEOF_INT128__)
  __uint128_t r=*A; r*=*B;
  #if(WYHASH_CONDOM>1)
  *A^=(u64)r; *B^=(u64)(r>>64);
  #else
  *A=(u64)r; *B=(u64)(r>>64);
  #endif
#elif defined(_MSC_VER) && defined(_M_X64)
  #if(WYHASH_CONDOM>1)
  u64  a,  b;
  a=_umul128(*A,*B,&b);
  *A^=a;  *B^=b;
  #else
  *A=_umul128(*A,*B,B);
  #endif
#else
  u64 ha=*A>>32, hb=*B>>32, la=(u32)*A, lb=(u32)*B, hi, lo;
  u64 rh=ha*hb, rm0=ha*lb, rm1=hb*la, rl=la*lb, t=rl+(rm0<<32), c=t<rl;
  lo=t+(rm1<<32); c+=lo<t; hi=rh+(rm0>>32)+(rm1>>32)+c;
  #if(WYHASH_CONDOM>1)
  *A^=lo;  *B^=hi;
  #else
  *A=lo;  *B=hi;
  #endif
#endif
}
static inline u64 _wymix(u64 A, u64 B){ _wymum(&A,&B); return A^B; }
static inline u64 _wyr8(const u8 *p) { u64 v; memcpy(&v, p, 8); return v;}
static inline u64 _wyr4(const u8 *p) { u32 v; memcpy(&v, p, 4); return v;}
static inline u64 _wyr3(const u8 *p, usize k) { return (((u64)p[0])<<16)|(((u64)p[k>>1])<<8)|p[k-1];}
static inline u64 wyhash(const void *key, usize len, u64 seed, const u64 *secret){
  const u8 *p=(const u8 *)key; seed^=_wymix(seed^secret[0],secret[1]);  u64  a,  b;
  if(LIKELY(len<=16)){
    if(LIKELY(len>=4)){ a=(_wyr4(p)<<32)|_wyr4(p+((len>>3)<<2)); b=(_wyr4(p+len-4)<<32)|_wyr4(p+len-4-((len>>3)<<2)); }
    else if(LIKELY(len>0)){ a=_wyr3(p,len); b=0;}
    else a=b=0;
  }
  else{
    usize i=len;
    if(UNLIKELY(i>48)){
      u64 see1=seed, see2=seed;
      do{
        seed=_wymix(_wyr8(p)^secret[1],_wyr8(p+8)^seed);
        see1=_wymix(_wyr8(p+16)^secret[2],_wyr8(p+24)^see1);
        see2=_wymix(_wyr8(p+32)^secret[3],_wyr8(p+40)^see2);
        p+=48; i-=48;
      }while(LIKELY(i>48));
      seed^=see1^see2;
    }
    while(UNLIKELY(i>16)){  seed=_wymix(_wyr8(p)^secret[1],_wyr8(p+8)^seed);  i-=16; p+=16;  }
    a=_wyr8(p+i-16);  b=_wyr8(p+i-8);
  }
  a^=secret[1]; b^=seed;  _wymum(&a,&b);
  return  _wymix(a^secret[0]^len,b^secret[1]);
}
//a useful 64bit-64bit mix function to produce deterministic pseudo random numbers that can pass BigCrush and PractRand
static inline u64 wyhash64(u64 A, u64 B){ A^=0xa0761d6478bd642full; B^=0xe7037ed1a0b428dbull; _wymum(&A,&B); return _wymix(A^0xa0761d6478bd642full,B^0xe7037ed1a0b428dbull);}
//The wyrand PRNG that pass BigCrush and PractRand
UNUSED static inline u64 wyrand(u64 *seed){ *seed+=0xa0761d6478bd642full; return _wymix(*seed,*seed^0xe7037ed1a0b428dbull);}

// ———————— end wyhash —————————


static u64 fastrand() {
  #if defined(__SIZEOF_INT128__) && __has_attribute(may_alias)
    // wyrand (https://github.com/wangyi-fudan/wyhash)
    // clang13 -O3 -mtune=native generates ~48 bytes (11 instrs) of x86_64 code for this
    typedef u64 __attribute__((__may_alias__)) u64a_t;
    fastrand_state += 0xa0761d6478bd642f;
    __uint128_t r =
      (__uint128_t)fastrand_state * (__uint128_t)(fastrand_state ^ 0xe7037ed1a0b428db);
    #if RSM_LITTLE_ENDIAN
      u64a_t hi = ((u64a_t*)&r)[0], lo = ((u64a_t*)&r)[1];
    #else
      u64a_t hi = ((u64a_t*)&r)[1], lo = ((u64a_t*)&r)[0];
    #endif
    return (u32)(hi ^ lo);
  #else
    return wyrand(&fastrand_state);
  #endif
}


static usize keyhash(const void *key, usize keysize, u64 seed) {
  static const u8 secret8[4];
  static const u64 secret[4] = {
    (u64)(uintptr)&secret8[0], (u64)(uintptr)&secret8[1],
    (u64)(uintptr)&secret8[2], (u64)(uintptr)&secret8[3] };
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
