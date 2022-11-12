// scope_t is used for tracking identifiers during parsing.
// This is a simple stack which we do a linear search on when looking up identifiers.
// It is faster than using chained hash maps in most cases because of cache locality
// and the fact that...
// 1. Most identifiers reference an identifier defined nearby. For example:
//      x = 3
//      A = x + 5
//      B = x - 5
// 2. Most bindings are short-lived and temporary ("locals") which means we can
//    simply change a single index pointer to "unwind" an entire scope of bindings and
//    then reuse that memory for the next binding scope.
//
// in scope_t, base is the offset in ptr to the current scope's base.
// Loading ptr[base] yields a uintptr that is the next scope's base index.
// keys (sym) and values (node) are interleaved in ptr together with saved base pointers.

#include "c0lib.h"
#include "compiler.h"


#define TRACE_SCOPESTACK

#ifdef TRACE_SCOPESTACK
  #define trace(s, fmt, args...) dlog("[scope %u] " fmt, level(s), args)
#else
  #define trace(s, fmt, args...) ((void)0)
#endif


void scope_clear(scope_t* s) {
  s->len = 0;
  s->base = 0;
}


void scope_dispose(scope_t* s, memalloc_t ma) {
  mem_freetv(ma, s->ptr, s->cap);
}


static u32 level(const scope_t* nullable s) {
  u32 n = 0;
  if (s) for (u32 base = s->base; base > 0; n++)
    base = (u32)(uintptr)s->ptr[base - 1];
  return n;
}


static bool scope_grow(scope_t* s, memalloc_t ma) {
  u32 initcap = 4;
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
  if UNLIKELY(s->len >= s->cap && !scope_grow(s, ma))
    return false;
  trace(s, "push: base %u -> %u", s->base, s->len);
  s->ptr[s->len++] = (void*)(uintptr)s->base;
  s->base = s->len;
  return true;
}


void scope_pop(scope_t* s) {
  #ifdef TRACE_SCOPESTACK
    u32 nbindings = (s->len - (s->base - 1)) / 2;
    trace(s, "pop: base %u -> %zu (%u bindings)",
      s->base, (usize)(uintptr)s->ptr[s->base - 1], nbindings);
    // scope_dlog(p);
  #endif
  // rewind and restore base of parent scope
  s->len = s->base - 1;
  s->base = (u32)(uintptr)s->ptr[s->len];
}


bool scope_def(scope_t* s, memalloc_t ma, const void* key, const void* value) {
  if UNLIKELY(s->cap - s->len < 2 && !scope_grow(s, ma))
    return false;
  trace(s, "def %p => %p", key, value);
  // note that key and value are entered in "reverse" order, which simplifies lookup
  s->ptr[s->len] = value;
  s->ptr[s->len + 1] = key;
  s->len += 2;
  return true;
}


const void* nullable scope_lookup(scope_t* s, const void* key) {
  u32 i = s->len;
  u32 base = s->base - 1;
  while (--i) {
    if (i == base) {
      base = (u32)(uintptr)s->ptr[i] - 1;
    } else if (s->ptr[i--] == key) {
      trace(s, "lookup %p => %p", key, s->ptr[i]);
      return s->ptr[i];
    }
  }
  trace(s, "lookup %p (not found)", key);
  return NULL;
}
