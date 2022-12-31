// SPDX-License-Identifier: Apache-2.0
#include "c0lib.h"
#include "compiler.h"


static map_t      symbols;
static memalloc_t sym_ma;

sym_t sym__;    // "_"
sym_t sym_this; // "this"
sym_t sym_drop; // "drop"
sym_t sym_main; // "main"


static sym_t defsym(const char* cstr) {
  void** vp = map_assign(&symbols, sym_ma, cstr, strlen(cstr));
  if UNLIKELY(!vp)
    panic("out of memory");
  *vp = mem_strdup(sym_ma, slice_cstr(cstr), 0);
  if UNLIKELY(!*vp)
    panic("out of memory");
  return *vp;
}


void sym_init(memalloc_t ma) {
  sym_ma = ma;
  if (!map_init(&symbols, sym_ma, 4096/sizeof(mapent_t)/2))
    panic("out of memory");
  sym__ = defsym("_");
  sym_this = defsym("this");
  sym_drop = defsym("drop");
  sym_main = defsym("main");
}


sym_t sym_intern(const void* key, usize keylen) {
  mapent_t* ent = map_assign_ent(&symbols, sym_ma, key, keylen);
  if UNLIKELY(!ent)
    goto oom;
  if (ent->value)
    return ent->value;
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
