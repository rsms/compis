// SPDX-License-Identifier: Apache-2.0
#include "colib.h"
#include "compiler.h"
#include "ast_field.h"

#define _(kind_, TYPE, enctag, NAME, size_) \
  static type_t _type_##NAME;
FOREACH_NODEKIND_PRIMTYPE(_)
#undef _

#define _(kind_, TYPE, enctag, NAME, size_) \
  type_t* type_##NAME = &_type_##NAME;
FOREACH_NODEKIND_PRIMTYPE(_)
#undef _


void universe_init() {
  #define _(kind_, TYPE, enctag, NAME, size_) { \
    static u32 typeid_##NAME[2] = {4, 0}; \
    typeid_##NAME[1] = g_ast_kindtagtab[kind_]; \
    _type_##NAME = (type_t){ \
      .kind = (kind_), \
      .is_builtin = true, \
      .flags = (NF_VIS_PUB | NF_CHECKED | ((kind_) == TYPE_UNKNOWN ? NF_UNKNOWN : 0)), \
      .size = (size_), \
      .align = (size_), \
      ._typeid = (const u8*)&typeid_##NAME[1], \
    }; \
    /*dlog("%s._typeid = \\x%02x%c%c%c%c", \
      nodekind_name(kind_), typeid[0], typeid[1], typeid[2], typeid[3], typeid[4]);*/ \
  }
  FOREACH_NODEKIND_PRIMTYPE(_)
  #undef _
}
