// SPDX-License-Identifier: Apache-2.0
#include "colib.h"
#include "sym.h"
#include "hashtable.h"
#include "hash.h"
#include "thread.h"
#include "ast.h"

#if DEBUG
#include "abuf.h"
extern usize strnlen(const char* s, usize maxlen); // libc
#endif

static strset_t   symbols;
static memalloc_t sym_ma;
static rwmutex_t  sym_mu;

#define FOREACH_PREDEFINED_SYMBOL(_/*(name)*/) \
  _(_) \
  _(this) \
  _(drop) \
  _(main) \
  _(str) \
  _(as) \
  _(from) \
  _(len) \
  _(cap) \
  _(reserve) \
// end FOREACH_PREDEFINED_SYMBOL

#define _(NAME) sym_t sym_##NAME;
FOREACH_PREDEFINED_SYMBOL(_)
#undef _

// primitive types
#define _(kind, TYPE, enctag, NAME, size) \
  sym_t sym_##NAME; \
  //sym_t sym_##NAME##_typeid;
FOREACH_NODEKIND_PRIMTYPE(_)
#undef _

sym_t _sym_primtype_nametab[PRIMTYPE_COUNT] = {0};


sym_t sym_intern(const char* key, usize keylen) {
  #ifdef DEBUG
    // check for prohibited bytes in key.
    // Note: AST encoder cannot handle symbols containing LF (0x0A '\n')
    char tmp[128];
    for (usize i = 0; i < keylen; i++) {
      u8 c = ((u8*)key)[i];
      if UNLIKELY(c == 0 || c == '\n') {
        int tmpz = (int)string_repr(tmp, sizeof(tmp), key, keylen);
        panic("symbol \"%.*s\" contains invalid byte at offset %zu: 0x%02x '%c'",
          tmpz, tmp, i, c, c);
      }
    }
  #endif

  // lookup under read-only lock
  rwmutex_rlock(&sym_mu);
  slice_t* ent = strset_lookup(&symbols, key, keylen);
  rwmutex_runlock(&sym_mu);
  if (!ent) {
    // not found; assign under full write lock
    rwmutex_lock(&sym_mu);
    ent = strset_assign(&symbols, key, keylen, NULL);
    rwmutex_unlock(&sym_mu);
    if UNLIKELY(!ent)
      goto oom;
  }
  return ent->chars;

oom:
  panic("out of memory");
}


sym_t sym_snprintf(char* buf, usize bufcap, const char* fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  usize len = vsnprintf(buf, bufcap, fmt, ap);
  sym_t s = sym_intern(buf, len);
  va_end(ap);
  return s;
}


static sym_t def_static_symn(const char* static_key, usize keylen) {
  assert(static_key[keylen] == 0); // must be NUL terminated

  #ifdef DEBUG
    bool added;
  #endif

  UNUSED slice_t* ent;

  ent = hashtable_assign(
    (hashtable_t*)&symbols,
    strset_hashfn,
    strset_eqfn,
    sizeof(slice_t),
    &(slice_t){ .p = static_key, .len = keylen },
    #ifdef DEBUG
      &added
    #else
      NULL
    #endif
  );

  safecheckf(ent, "out of memory");
  assertf(added, "%s", static_key);
  assert(ent->len == keylen);
  assert(ent->p == static_key);

  return static_key;
}


inline static sym_t def_static_sym(const char* static_cstr) {
  return def_static_symn(static_cstr, strlen(static_cstr));
}


void sym_init(memalloc_t ma) {
  safecheckx(rwmutex_init(&sym_mu) == 0);
  sym_ma = ma;
  UNUSED err_t err = strset_init(&symbols, ma, 4096/sizeof(slice_t)/2);
  safecheckf(err == 0, "strset_init: %s", err_str(err));

  #define _(NAME) sym_##NAME = def_static_sym(#NAME);
  FOREACH_PREDEFINED_SYMBOL(_)
  #undef _

  // static const char typeid_symtab[PRIMTYPE_COUNT][2] = {
  //   #define _(kind, TYPE, enctag, NAME, size)  {TYPEID_PREFIX(kind),0},
  //   FOREACH_NODEKIND_PRIMTYPE(_)
  //   #undef _
  // };

  // // sym_NAME = "NAME"  (e.g. sym_int = "int")
  // #define _(kind, TYPE, enctag, NAME, size) \
  //   sym_##NAME = def_static_sym(#NAME); \
  //   sym_##NAME##_typeid = def_static_symn(typeid_symtab[kind - TYPE_VOID], 1); \
  //   _sym_primtype_nametab[kind - TYPE_VOID] = sym_##NAME;
  // FOREACH_NODEKIND_PRIMTYPE(_)
  // #undef _

  // sym_NAME = "NAME"  (e.g. sym_int = "int")
  #define _(kind, TYPE, enctag, NAME, size) \
    sym_##NAME = def_static_sym(#NAME); \
    _sym_primtype_nametab[kind - TYPE_VOID] = sym_##NAME;
  FOREACH_NODEKIND_PRIMTYPE(_)
  #undef _
}
