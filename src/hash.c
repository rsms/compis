// SPDX-License-Identifier: Apache-2.0
#include "colib.h"
#include "hash.h"

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

u64 wyhash(const void *key, usize len, u64 seed, const u64 *secret) {
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

// a useful 64bit-64bit mix function to produce deterministic pseudo random numbers that can pass BigCrush and PractRand
u64 wyhash64(u64 A, u64 B) {
  A ^= 0xa0761d6478bd642full;
  B ^= 0xe7037ed1a0b428dbull;
  _wymum(&A, &B);
  return _wymix(A ^ 0xa0761d6478bd642full, B ^ 0xe7037ed1a0b428dbull);
}

// wyrand for fastrand impl
#if defined(__SIZEOF_INT128__) && __has_attribute(may_alias)
#else
  // The wyrand PRNG that pass BigCrush and PractRand
  static inline u64 wyrand(u64 *seed) {
    *seed += 0xa0761d6478bd642full;
    return _wymix(*seed, *seed ^ 0xe7037ed1a0b428dbull);
  }
#endif

// ———————— end wyhash —————————


static u64 fastrand_state = (u64)(uintptr)&fastrand_state;


u64 fastrand() {
  #if defined(__SIZEOF_INT128__) && __has_attribute(may_alias)
    // wyrand with __uint128_t
    // clang13 -O3 -mtune=native generates 11 instructions of x86_64 code for this
    typedef u64 __attribute__((__may_alias__)) u64a_t;
    fastrand_state += 0xa0761d6478bd642full;
    __uint128_t r =
      (__uint128_t)fastrand_state *
      (__uint128_t)(fastrand_state ^ 0xe7037ed1a0b428dbull);
    #if CO_LITTLE_ENDIAN
      u64a_t hi = ((u64a_t*)&r)[0], lo = ((u64a_t*)&r)[1];
    #else
      u64a_t hi = ((u64a_t*)&r)[1], lo = ((u64a_t*)&r)[0];
    #endif
    return hi ^ lo;
  #else
    return wyrand(&fastrand_state);
  #endif
}
