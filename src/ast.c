// SPDX-License-Identifier: Apache-2.0
#include "colib.h"
#include "ast.h"

// node kind string table with compressed indices (compared to table of pointers.)
// We end up with something like this; one string with indices:
//   enum {
//     NK_NBAD, NK__NBAD = NK_NBAD + strlen("NBAD"),
//     NK_NCOMMENT, NK__NCOMMENT = NK_NCOMMENT + strlen("NCOMMENT"),
//     NK_NUNIT, NK__NUNIT = NK_NUNIT + strlen("NUNIT"),
//     NK_NFUN, NK__NFUN = NK_NFUN + strlen("NFUN"),
//     NK_NBLOCK, NK__NBLOCK = NK_NBLOCK + strlen("NBLOCK"),
//     NK_UNKNOWN, NK__UNKNOWN = NK_UNKNOWN + strlen("N?"),
//   };
//   static const struct { u8 offs[NODEKIND_COUNT]; char strs[]; } nodekind_strtab = {
//     { NK_NBAD, NK_NCOMMENT, NK_NUNIT, NK_NFUN, NK_NBLOCK },
//     { "NBAD\0NCOMMENT\0NUNIT\0NFUN\0NBLOCK\0N?" }
//   };
#define NK_UNKNOWN_STR "NODE_???"
enum {
  #define _(NAME, ...) NK_##NAME, NK__##NAME = NK_##NAME + strlen(#NAME),
  FOREACH_NODEKIND(_)
  #undef _
  NK_UNKNOWN, NK__UNKNOWN = NK_UNKNOWN + strlen(NK_UNKNOWN_STR),
};
static const struct {
  int  offs[NODEKIND_COUNT + 1]; // index into strs
  char strs[];
} nodekind_strtab = {
  { // get offset from enum
    #define _(NAME, ...) NK_##NAME,
    FOREACH_NODEKIND(_)
    #undef _
    NK_UNKNOWN,
  }, {
    #define _(NAME, ...) #NAME "\0"
    FOREACH_NODEKIND(_)
    #undef _
    NK_UNKNOWN_STR
  }
};


#define STRTAB_GET(strtab, kind) \
  &(strtab).strs[ (strtab).offs[ MIN((kind), countof(strtab.offs)-1) ] ]


const char* nodekind_name(nodekind_t kind) {
  return STRTAB_GET(nodekind_strtab, kind);
}


const char* node_srcfilename(const node_t* n, locmap_t* lm) {
  const srcfile_t* sf = n->loc != 0 ? loc_srcfile(n->loc, lm) : NULL;
  return sf ? sf->name.p : "<input>";
}


bool ast_is_main_fun(const fun_t* fn) {
  return
    fn->kind == EXPR_FUN &&
    fn->recvt == NULL &&
    fn->name == sym_main &&
    (fn->flags & NF_VIS_PUB) &&
    (fn->nsparent != NULL && fn->nsparent->kind == NODE_UNIT);
}
