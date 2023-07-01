// SPDX-License-Identifier: Apache-2.0
#include "colib.h"
#include "compiler.h"


err_t typefuntab_init(typefuntab_t* tfuns, memalloc_t ma) {
  err_t err = rwmutex_init(&tfuns->mu);
  if (err)
    return err;
  if (!map_init(&tfuns->m, ma, 8)) {
    rwmutex_dispose(&tfuns->mu);
    return ErrNoMem;
  }
  return 0;
}


void typefuntab_dispose(typefuntab_t* tfuns, memalloc_t ma) {
  for (const mapent_t* e = map_it(&tfuns->m); map_itnext(&tfuns->m, &e); )
    map_dispose((map_t*)e->value, ma);
  map_dispose(&tfuns->m, ma);
}


fun_t* nullable typefuntab_lookup(typefuntab_t* tfuns, type_t* t, sym_t name) {
  // look for type function, considering alias types.
  //   1. unwrap optional, ref and ptr type so that e.g. &MyMyT becomes MyMyT.
  //   2. Lookup MyT.name in tfuns (alias of T), if found return the function.
  //   3. if MyT is an alias, unwrap MyT => T, repeat steps 1–3
  //   4. not found; return NULL

  fun_t* fun = NULL;

  rwmutex_rlock(&tfuns->mu);

  for (;;) {
    t = type_unwrap_ptr(t); // e.g. "&T" => "T"
    map_t** mp = (map_t**)map_lookup_ptr(&tfuns->m, typeid(t));
    if (mp) {
      // *mp is a map that maps {sym_t name => fun_t*}
      assertnotnull(*mp);
      fun_t** fnp = (fun_t**)map_lookup_ptr(*mp, name);
      if (fnp) {
        assert((*fnp)->kind == EXPR_FUN);
        fun = *fnp;
        break;
      }
    }
    // not found
    // if t is an alias, look for a type function defined for its underlying type
    if (t->kind != TYPE_ALIAS)
      break;
    t = assertnotnull(((aliastype_t*)t)->elem);
  }

  rwmutex_runlock(&tfuns->mu);
  return fun;
}
