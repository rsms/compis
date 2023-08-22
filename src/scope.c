// scope is used for tracking identifiers during parsing
// SPDX-License-Identifier: Apache-2.0
//
#if __documentation__
// A scope_t is a stack which we search when looking up identifiers.
// In practice it is usually faster than using chained hash maps
// because of cache locality and the fact that...
//   1. Most identifiers reference an identifier defined nearby. For example:
//        x = 3
//        A = x + 5
//        B = x - 5
//   2. Most bindings are short-lived which means we can simply change
//      a single index pointer to "unwind" an entire scope of bindings
//      and then reuse that memory for the next binding scope.
//
// In scope_t, base is the offset in ptr to the current scope's base.
// Loading ptr[base] yields the next scope's base index.
// Keys and values are interleaved.
//
// Example of all operations:
//
// │ pseudo code       │ state after operation on the left
// ├───────────────────┼──────────────────────────────────────────────────
 0 │                   │ base=0 len=0  []
 1 │ { push            │ base=0 len=1  [0]
 2 │   def A 1         │ base=0 len=3  [0 1,A]
 3 │   { push          │ base=3 len=4  [0 1,A  0]
 4 │     def B 2       │ base=3 len=6  [0 1,A  0 2,B]
 5 │     { push        │ base=6 len=7  [0 1,A  0 2,B  3]
 6 │       def C 3     │ base=6 len=9  [0 1,A  0 2,B  3 3,C]
   │                   │ //       Note: Parent base ──┘
 7 │     } pop         │ base=3 len=6  [0 1,A  0 2,B]
 8 │   } pop           │ base=0 len=3  [0 1,A]
 9 │   { push          │ base=3 len=4  [0 1,A  0]
10 │     def D 4       │ base=3 len=6  [0 1,A  0 4,D]
11 │     lookup D      │ // D==D? => found
12 │   } stash         │ base=7 len=8  [0 1,A  0 4,D  3,STASH]
   │                   │ //       Note: Parent base ──┘
13 │   { push          │ base=8 len=9  [0 1,A  0 4,D  3,STASH  7]
14 │     def E 5       │ base=8 len=11 [0 1,A  0 4,D  3,STASH  7 5,e]
15 │     lookup D      │ // E==D?, A==D? => not found
16 │   } pop           │ base=7 len=8  [0 1,A  0 4,D  3,STASH]
17 │   unstash         │ base=3 len=6  [0 1,A  0 4,D]
18 │   lookup D        │ // D==D? => found
19 │   pop             │ base=0 len=3  [0 1,A]
20 │ } pop             │ base=0 len=0  []
//
//
// Example of lookup, based on state from the above example:
//
state
  //              0 1 2  3 4 5  6 7      8 9 10
  base=8 len=11  [0 1,A  0 4,D  3,STASH  7 5,E]
  //                     ↑      │
  //                     └──────┘ Parent base
  //
lookup(A)
  i = 11               // i=len
  if i > 2
    i = 10             // i--
    if i == base       // 10 == 8 : false
    if [i] == key      // E == A : false
    i = 9              // i--
  if i > 2
    i = 8              // i--
    if i == base       // 8 == 8 : true
      if [i] == STASH  // [8] == STASH : false
      base = 7         // base = [i]
  if i > 2
    i = 7              // i--
    if i == base       // 7 == 7 : true
      if [i] == STASH  // [i] == STASH : true
        i = 3          // [i - 1] = [6]
      base = 0         // base = [i]
  if i > 2
    i = 2              // i--
    if i == base       // 2 == 0 : false
    if [i] == key      // A == A : true
      return [1]       // value = [i-1]

// algorithms:

lookup(key)
  i = len
  while i > 2
    i--
    if i == base
      if [i] == STASH
        i = [i - 1]  // skip over stashed scope
      base = [i]
    else
      if [i] == key
        return value = [i]
      i--
  return not_found

define(key, value)
  [len] = value
  [len + 1] = key
  len += 2

push()
  [len] = base
  base = len
  len++

pop()
  assert([base] != STASH)
  len = base
  base = [base]

stash()
  [len] = base
  [len + 1] = STASH
  base = len + 1
  len += 2

unstash()
  assert([len - 1] == STASH)
  base = [len - 2]
  len -= 2

//
#endif //__documentation__
//—————————————————————————————————————————————————————————————————————————————————————

#include "colib.h"
#include "compiler.h"

// address used to mark a stashed scope
static u8 kStash;

//—————————————————————————————————————————————————————————————————————————————————————
// TRACE_SCOPESTACK: define to trace details on stderr
//#define TRACE_SCOPESTACK

#define TRACE_KEY_FMT  "\"%s\""
#define TRACE_KEY(key) ((const char*)(key))

#if defined(TRACE_SCOPESTACK) && defined(DEBUG)
  #define trace(s, fmt, args...) \
    _trace(true, 7, "scope", "[%u] " fmt, scope_level(s), ##args)
  static void trace_state(const scope_t* nullable s) {
    fprintf(stderr, "(len %u, base %u) ", s->len, s->base);
    if (!s)          return fprintf(stderr, "null\n"), ((void)0);
    if (s->len == 0) return fprintf(stderr, "{}\n"), ((void)0);
    if (s->len < 3)  return fprintf(stderr, "bad:len<3\n"), ((void)0);
    fprintf(stderr, "{");
    u32 i = s->len;
    u32 base = s->base;
    u32 stashstk[16];
    u32 stashstk_len = 0;
    while (i > 2) {
      i--;
      if (i == base && stashstk_len && stashstk[stashstk_len - 1] == base)
        stashstk_len--;
      fprintf(stderr, "\n %*s ", (int)stashstk_len*2, "");
      if (i == base) {
        if (s->ptr[i] == &kStash) {
          assert(i > 2);
          u32 endi = i;

          u32 pbase = (u32)(uintptr)s->ptr[i - 1];
          i--;
          base = (u32)(uintptr)s->ptr[i];
          fprintf(stderr, "[%2u…%-2u] STASH base=%u", endi, i, pbase);
          if (stashstk_len < countof(stashstk))
            stashstk[stashstk_len++] = base;
        } else {
          base = (u32)(uintptr)s->ptr[i];
          fprintf(stderr, "[%2u   ] BASE  %u", i, base);
        }
      } else {
        fprintf(stderr, "[%2u…%-2u] ENTRY " TRACE_KEY_FMT " => %p",
          i, i - 1, TRACE_KEY(s->ptr[i]), s->ptr[i - 1]);
        i--;
      }
    }
    fprintf(stderr, "\n  [ 0   ] BASE(%u)", (u32)(uintptr)s->ptr[0]);
    fprintf(stderr, "\n}\n");
  }
#else
  #undef TRACE_SCOPESTACK
  #define trace(s, fmt, args...) ((void)0)
  #define trace_state(...)        ((void)0)
#endif


//—————————————————————————————————————————————————————————————————————————————————————


void scope_clear(scope_t* s) {
  s->len = 0;
  s->base = 0;
}


void scope_dispose(scope_t* s, memalloc_t ma) {
  mem_freetv(ma, s->ptr, s->cap);
}


// bool scope_copy(scope_t* dst, const scope_t* src, memalloc_t ma) {
//   void* ptr = mem_allocv(ma, src->cap, sizeof(void*));
//   if (!ptr)
//     return false;
//   memcpy(ptr, src->ptr, src->cap * sizeof(void*));
//   *dst = *src;
//   dst->ptr = ptr;
//   return true;
// }


static bool scope_grow(scope_t* s, memalloc_t ma) {
  u32 initcap = 16;
  u32 newcap = (s->cap + ((initcap/2) * !s->cap)) * 2;

  trace(s, "grow: cap %u -> %u", s->cap, newcap);

  void* newptr = mem_resizev(ma, s->ptr, s->cap, newcap, sizeof(void*));
  if UNLIKELY(!newptr)
    return false;
  s->ptr = newptr;
  s->cap = newcap;
  return true;
}


bool scope_push(scope_t* s, memalloc_t ma) {
  trace(s, "push: base %u -> %u", s->base, s->len);

  if UNLIKELY(s->len >= s->cap && !scope_grow(s, ma))
    return false;
  s->ptr[s->len] = (void*)(uintptr)s->base;
  s->base = s->len;
  s->len++;

  trace_state(s);
  return true;
}


void scope_pop(scope_t* s) {
  // rewind and restore base of parent scope
  trace(s, "pop: base %u -> %zu (%u bindings)",
    s->base, (usize)(uintptr)s->ptr[s->base],
    (s->len - (s->base - 1)) / 2 );
  assertf(s->ptr[s->base] != &kStash,
    "has stashed scope (forgot to call scope_unstash)");

  s->len = s->base;
  s->base = (u32)(uintptr)s->ptr[s->len];

  trace_state(s);
}


bool scope_stash(scope_t* s, memalloc_t ma) {
  trace(s, "stash base=%u", s->base);

  if UNLIKELY(s->cap - s->len < 2 && !scope_grow(s, ma))
    return false;

  s->ptr[s->len] = (void*)(uintptr)s->base;
  s->ptr[s->len + 1] = &kStash;
  s->base = s->len + 1;
  s->len += 2;

  trace_state(s);
  return true;
}


void scope_unstash(scope_t* s) {
  trace(s, "unstash");
  assertf(s->len > 0 && s->ptr[s->len-1] == &kStash, "no stashed scope");

  s->base = (u32)(uintptr)s->ptr[s->len - 2];
  s->len -= 2;

  trace_state(s);
}


void* nullable scope_lookup(scope_t* s, const void* key, u32 maxdepth) {
  trace(s, "lookup " TRACE_KEY_FMT, TRACE_KEY(key));
  u32 i = s->len;
  u32 base = s->base;
  while (i > 2) {
    i--;
    if (i == base) {
      if (maxdepth == 0)
        break;
      maxdepth--;
      if (s->ptr[i] == &kStash) {
        trace(s, "  [%2u] => stash (base=%u)", i, (u32)(uintptr)s->ptr[i - 1]);
        i = (u32)(uintptr)s->ptr[i - 1]; // base of stashed scope
      }
      base = (u32)(uintptr)s->ptr[i];
      trace(s, "  [%2u] => base %u", i, base);
    } else {
      trace(s, "  [%2u] test " TRACE_KEY_FMT " == " TRACE_KEY_FMT " (%s)",
        i, TRACE_KEY(s->ptr[i]), TRACE_KEY(key),
        s->ptr[i] == key ? "match" : "no match");
      if (s->ptr[i] == key)
        return s->ptr[i - 1];
      i--;
    }
  }
  trace(s, "  not found");
  return NULL;
}


bool scope_undefine(scope_t* s, memalloc_t ma, const void* key) {
  trace(s, "undefine " TRACE_KEY_FMT, TRACE_KEY(key));
  u32 i = s->len;
  u32 base = s->base;
  while (i > 2) {
    i--;
    if (i == base)
      break;
    void* k = s->ptr[i];
    i--;
    if (k == key) {
      if (i + 2 < s->len) {
        void* dst = s->ptr + i;
        void* src = dst + 2;
        memmove(dst, src, (s->len - i - 2));
      }
      s->len -= 2;
      return true;
    }
  }
  trace(s, "  not found");
  return false;
}


bool scope_define(scope_t* s, memalloc_t ma, const void* key, void* value) {
  trace(s, "define " TRACE_KEY_FMT " => %p", TRACE_KEY(key), value);

  if UNLIKELY(s->cap - s->len < 2 && !scope_grow(s, ma))
    return false;

  // note that key and value are entered in "reverse" order, which simplifies lookup
  s->ptr[s->len] = value;
  s->ptr[s->len + 1] = (void*)key;
  s->len += 2;

  trace_state(s);
  return true;
}


void scope_iterate(scope_t* s, u32 maxdepth, scopeit_t it, void* nullable ctx) {
  u32 i = s->len;
  u32 base = s->base;
  while (i > 2) {
    i--;
    if (i == base) {
      if (maxdepth == 0)
        break;
      maxdepth--;
      if (s->ptr[i] == &kStash)
        i = (u32)(uintptr)s->ptr[i - 1]; // base of stashed scope
      base = (u32)(uintptr)s->ptr[i];
    } else {
      if (!it(s->ptr[i], s->ptr[i - 1], ctx))
        break;
      i--;
    }
  }
}


u32 scope_level(const scope_t* nullable s) {
  u32 n = 0;
  if (s && s->len > 2) for (u32 base = s->base; base > 0; n++) {
    if (s->ptr[base] == &kStash) {
      assert(base > 1);
      u32 base2 = (u32)(uintptr)s->ptr[base - 1];
      assertf(base2 < base || base2 == 0, "%u < %u", base2, base);
      base = base2;
    } else {
      u32 base2 = (u32)(uintptr)s->ptr[base];
      assertf(base2 < base || base2 == 0, "%u < %u", base2, base);
      base = base2;
    }
  }
  return n;
}
