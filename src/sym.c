// SPDX-License-Identifier: Apache-2.0
#include "colib.h"
#include "compiler.h"

#if DEBUG
#include "abuf.h"
extern usize strnlen(const char* s, usize maxlen); // libc
#endif

static map_t      symbols;
static memalloc_t sym_ma;

sym_t sym__;    // "_"
sym_t sym_this; // "this"
sym_t sym_drop; // "drop"
sym_t sym_main; // "main"
sym_t sym_str;  // "str"
sym_t sym_as;   // "as"
sym_t sym_from; // "from"

// primitive types
#define _(kind, TYPE, enctag, NAME, size) \
  sym_t sym_##NAME; \
  sym_t sym_##NAME##_typeid;
FOREACH_NODEKIND_PRIMTYPE(_)
#undef _

sym_t _primtype_nametab[PRIMTYPE_COUNT] = {0};


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

  mapent_t* ent = map_assign_ent(&symbols, sym_ma, key, keylen);
  if UNLIKELY(!ent)
    goto oom;

  if (ent->value) // already exist
    return ent->value;

  // dlog("create symbol '%.*s' %p", (int)keylen, (const char*)key, ent);

  ent->value = mem_strdup(sym_ma, (slice_t){ .p=key, .len=keylen }, 0);
  if UNLIKELY(!ent->value)
    goto oom;
  // update key as the key parameter is a borrowed ref
  ent->key = ent->value;
  ent->keysize = keylen;
  return ent->value;
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
  mapent_t* ent = map_assign_ent(&symbols, sym_ma, static_key, keylen);
  if UNLIKELY(!ent)
    panic("out of memory");
  assert(ent->value == NULL);
  ent->value = (void*)static_key;
  ent->key = static_key;
  ent->keysize = keylen;
  return static_key;
}


inline static sym_t def_static_sym(const char* static_cstr) {
  return def_static_symn(static_cstr, strlen(static_cstr));
}


void sym_init(memalloc_t ma) {
  sym_ma = ma;
  if (!map_init(&symbols, sym_ma, 4096/sizeof(mapent_t)/2))
    panic("out of memory");
  sym__ = def_static_sym("_");
  sym_this = def_static_sym("this");
  sym_drop = def_static_sym("drop");
  sym_main = def_static_sym("main");
  sym_str  = def_static_sym("str");
  sym_as   = def_static_sym("as");
  sym_from = def_static_sym("from");

  static const char typeid_symtab[PRIMTYPE_COUNT][2] = {
    #define _(kind, TYPE, enctag, NAME, size)  {TYPEID_PREFIX(kind),0},
    FOREACH_NODEKIND_PRIMTYPE(_)
    #undef _
  };

  // sym_NAME = "NAME"  (e.g. sym_int = "int")
  #define _(kind, TYPE, enctag, NAME, size) \
    sym_##NAME = def_static_sym(#NAME); \
    sym_##NAME##_typeid = def_static_symn(typeid_symtab[kind - TYPE_VOID], 1); \
    _primtype_nametab[kind - TYPE_VOID] = sym_##NAME;
  FOREACH_NODEKIND_PRIMTYPE(_)
  #undef _
}
