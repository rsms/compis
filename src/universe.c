// SPDX-License-Identifier: Apache-2.0
#include "colib.h"
#include "compiler.h"

#define _(kind_, TYPE, enctag, NAME, size_) \
  static type_t _type_##NAME;
FOREACH_NODEKIND_PRIMTYPE(_)
#undef _

#define _(kind_, TYPE, enctag, NAME, size_) \
  type_t* type_##NAME = &_type_##NAME;
FOREACH_NODEKIND_PRIMTYPE(_)
#undef _


void universe_init() {
  // these can't be initialized at compile time since they use sym_*_typeid

  #define _(kind_, TYPE, enctag, NAME, size_) \
    _type_##NAME = (type_t){ \
      .kind = (kind_), \
      .flags = (NF_VIS_PUB | NF_CHECKED | ((kind_) == TYPE_UNKNOWN ? NF_UNKNOWN : 0)), \
      .size = (size_), \
      .align = (size_), \
      .tid = assertnotnull(sym_##NAME##_typeid), \
    };

  FOREACH_NODEKIND_PRIMTYPE(_)
  #undef _
}
