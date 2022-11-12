#include "c0lib.h"
#include "compiler.h"


static map_t      symbols;
static memalloc_t sym_ma;

sym_t sym__; // "_"


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
  defsym("_");
}


sym_t sym_intern(const void* key, usize keylen) {
  void** vp = map_assign(&symbols, sym_ma, key, keylen);
  if UNLIKELY(!vp)
    goto oom;
  if UNLIKELY(*vp == NULL) {
    *vp = mem_strdup(sym_ma, (slice_t){ .p=key, .len=keylen }, 0);
    if UNLIKELY(!*vp)
      goto oom;
  }
  return *vp;
oom:
  panic("out of memory");
}
