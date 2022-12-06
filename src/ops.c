// SPDX-License-Identifier: Apache-2.0
#include "c0lib.h"
#include "compiler.h"

enum _op_count {
  #define _(NAME, ...) _x##NAME,
  #include "ops.h"
  #undef _
  OP_COUNT_
};

// ops string table with compressed indices (compared to table of pointers.)
// We end up with something like this; one string with indices:
//   enum {
//     OPSTR_NBAD, OPSTR__NBAD = OPSTR_NBAD + strlen("NBAD"),
//     OPSTR_NCOMMENT, OPSTR__NCOMMENT = OPSTR_NCOMMENT + strlen("NCOMMENT"),
//     OPSTR_NUNIT, OPSTR__NUNIT = OPSTR_NUNIT + strlen("NUNIT"),
//     OPSTR_NFUN, OPSTR__NFUN = OPSTR_NFUN + strlen("NFUN"),
//     OPSTR_NBLOCK, OPSTR__NBLOCK = OPSTR_NBLOCK + strlen("NBLOCK"),
//     OPSTR_UNKNOWN, OPSTR__UNKNOWN = OPSTR_UNKNOWN + strlen("N?"),
//   };
//   static const struct { u8 offs[NODEKIND_COUNT]; char strs[]; } nodekind_strtab = {
//     { OPSTR_NBAD, OPSTR_NCOMMENT, OPSTR_NUNIT, OPSTR_NFUN, OPSTR_NBLOCK },
//     { "NBAD\0NCOMMENT\0NUNIT\0NFUN\0NBLOCK\0N?" }
//   };
#define OPSTR_UNKNOWN_STR "OP_?"
enum {
  #define _(NAME, ...) OPSTR_##NAME, OPSTR__##NAME = OPSTR_##NAME + strlen(#NAME),
  #include "ops.h"
  #undef _
  OPSTR_UNKNOWN, OPSTR__UNKNOWN = OPSTR_UNKNOWN + strlen(OPSTR_UNKNOWN_STR),
};
static const struct {
  u32  offs[OP_COUNT_ + 1]; // index into strs
  char strs[];
} op_strtab = {
  { // get offset from enum
    #define _(NAME, ...) OPSTR_##NAME,
    #include "ops.h"
    #undef _
    OPSTR_UNKNOWN,
  }, {
    #define _(NAME, ...) #NAME "\0"
    #include "ops.h"
    #undef _
    OPSTR_UNKNOWN_STR
  }
};

#define STRTAB_GET(strtab, index) \
  &(strtab).strs[ (strtab).offs[ MIN(((usize)index), countof((strtab).offs)-1) ] ]


const char* op_name(op_t op) {
  return STRTAB_GET(op_strtab, op);
}


int op_name_maxlen() {
  static u32 w = 0;
  if (w == 0) {
    u32 offs = 0;
    for (usize i = 0; i < countof(op_strtab.offs); i++) {
      w = MAX(w, op_strtab.offs[i] - offs);
      offs = op_strtab.offs[i];
    }
  }
  return (int)w;
}

