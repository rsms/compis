// SPDX-License-Identifier: Apache-2.0
#include "colib.h"
#include "ops.h"
#undef CO_INCLUDE_OPS

enum { OP_COUNT_ = (0lu
  #define _(NAME, ...) +1
  #include "ops.h"
  #undef _
  #undef CO_INCLUDE_OPS
) };

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
  #undef CO_INCLUDE_OPS
  OPSTR_UNKNOWN, OPSTR__UNKNOWN = OPSTR_UNKNOWN + strlen(OPSTR_UNKNOWN_STR),
};
static const struct {
  u32  offs[OP_COUNT_ + 1]; // index into strs
  char strs[];
} op_strtab = {
  { // get offset from enum
    #define _(NAME, ...) OPSTR_##NAME,
    #include "ops.h"
    #undef CO_INCLUDE_OPS
    #undef _
    OPSTR_UNKNOWN,
  }, {
    #define _(NAME, ...) #NAME "\0"
    #include "ops.h"
    #undef CO_INCLUDE_OPS
    #undef _
    OPSTR_UNKNOWN_STR
  }
};


#define FMT_UNKNOWN_STR ""
enum {
  #define _(NAME, fmtstr) FMT_##NAME, FMT__##NAME = FMT_##NAME + strlen(fmtstr),
  #include "ops.h"
  #undef CO_INCLUDE_OPS
  #undef _
  FMT_UNKNOWN, FMT__UNKNOWN = FMT_UNKNOWN + strlen(FMT_UNKNOWN_STR),
};
static const struct {
  u32  offs[OP_COUNT_ + 1]; // index into strs
  char strs[];
} op_fmttab = {
  { // get offset from enum
    #define _(NAME, ...) FMT_##NAME,
    #include "ops.h"
    #undef CO_INCLUDE_OPS
    #undef _
    FMT_UNKNOWN,
  }, {
    #define _(NAME, fmtstr) fmtstr "\0"
    #include "ops.h"
    #undef CO_INCLUDE_OPS
    #undef _
    FMT_UNKNOWN_STR
  }
};



#define STRTAB_GET(strtab, index) \
  &(strtab).strs[ (strtab).offs[ MIN(((usize)index), countof((strtab).offs)-1) ] ]


const char* op_name(op_t op) {
  return STRTAB_GET(op_strtab, op);
}


const char* op_fmt(op_t op) {
  const char* s = STRTAB_GET(op_fmttab, op);
  return *s ? s : op_name(op);
}


int op_name_maxlen() {
  int w = 0;
  #define _(NAME, fmtstr) if ((int)strlen(fmtstr) > w) w = (int)strlen(fmtstr);
  #include "ops.h"
  #undef CO_INCLUDE_OPS
  #undef _
  return w;
}
