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

// primitive types
sym_t sym_void;
sym_t sym_bool;
sym_t sym_int;
sym_t sym_uint;
sym_t sym_i8;
sym_t sym_i16;
sym_t sym_i32;
sym_t sym_i64;
sym_t sym_u8;
sym_t sym_u16;
sym_t sym_u32;
sym_t sym_u64;
sym_t sym_f32;
sym_t sym_f64;

sym_t _primtype_nametab[(TYPE_F64 - TYPE_VOID) + 1] = {0};

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
  sym_str  = defsym("str");

  sym_void = defsym("void");
  sym_bool = defsym("bool");
  sym_int = defsym("int");
  sym_uint = defsym("uint");
  sym_i8 = defsym("i8");
  sym_i16 = defsym("i16");
  sym_i32 = defsym("i32");
  sym_i64 = defsym("i64");
  sym_u8 = defsym("u8");
  sym_u16 = defsym("u16");
  sym_u32 = defsym("u32");
  sym_u64 = defsym("u64");
  sym_f32 = defsym("f32");
  sym_f64 = defsym("f64");

  _primtype_nametab[TYPE_VOID - TYPE_VOID] = sym_void;
  _primtype_nametab[TYPE_BOOL - TYPE_VOID] = sym_bool;
  _primtype_nametab[TYPE_I8 - TYPE_VOID]   = sym_i8;
  _primtype_nametab[TYPE_I16 - TYPE_VOID]  = sym_i16;
  _primtype_nametab[TYPE_I32 - TYPE_VOID]  = sym_i32;
  _primtype_nametab[TYPE_I64 - TYPE_VOID]  = sym_i64;
  _primtype_nametab[TYPE_INT - TYPE_VOID]  = sym_int;
  _primtype_nametab[TYPE_U8 - TYPE_VOID]   = sym_u8;
  _primtype_nametab[TYPE_U16 - TYPE_VOID]  = sym_u16;
  _primtype_nametab[TYPE_U32 - TYPE_VOID]  = sym_u32;
  _primtype_nametab[TYPE_U64 - TYPE_VOID]  = sym_u64;
  _primtype_nametab[TYPE_UINT - TYPE_VOID] = sym_uint;
  _primtype_nametab[TYPE_F32 - TYPE_VOID]  = sym_f32;
  _primtype_nametab[TYPE_F64 - TYPE_VOID]  = sym_f64;
}


sym_t sym_intern(const void* key, usize keylen) {
  #if DEBUG
    if UNLIKELY(strnlen(key, keylen) != keylen) {
      char tmp[128];
      abuf_t s = abuf_make(tmp, sizeof(tmp));
      abuf_repr(&s, (char*)key, keylen);
      abuf_terminate(&s);
      assertf(0, "symbol \"%s\" contains NUL byte (len %zu, nul at %zu)",
        tmp, keylen, strnlen(key, keylen));
    }
  #endif

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
