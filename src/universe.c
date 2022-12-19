// SPDX-License-Identifier: Apache-2.0
#include "c0lib.h"
#include "compiler.h"

#define DEFTYPE(_kind, _flags, _size, _isunsigned) \
  (type_t*)&(const type_t){ \
    .kind = (_kind), \
    .flags = (_flags), \
    .size = (_size), \
    .align = (_size), \
    .isunsigned = (_isunsigned), \
    .tid = (char[1]){TYPEID_PREFIX(_kind)}, \
  }

type_t* type_void = DEFTYPE(TYPE_VOID, 0, 0, false);
type_t* type_unknown = DEFTYPE(TYPE_UNKNOWN, NF_UNKNOWN, 0, false);

type_t* type_bool  = DEFTYPE(TYPE_BOOL, 0, 1, true);

type_t* type_int  = DEFTYPE(TYPE_INT, 0, 4, false);
type_t* type_uint = DEFTYPE(TYPE_INT, 0, 4, true);

type_t* type_i8  = DEFTYPE(TYPE_I8, 0,  1, false);
type_t* type_i16 = DEFTYPE(TYPE_I16, 0, 2, false);
type_t* type_i32 = DEFTYPE(TYPE_I32, 0, 4, false);
type_t* type_i64 = DEFTYPE(TYPE_I64, 0, 8, false);

type_t* type_u8  = DEFTYPE(TYPE_I8, 0,  1, true);
type_t* type_u16 = DEFTYPE(TYPE_I16, 0, 2, true);
type_t* type_u32 = DEFTYPE(TYPE_I32, 0, 4, true);
type_t* type_u64 = DEFTYPE(TYPE_I64, 0, 8, true);

type_t* type_f32 = DEFTYPE(TYPE_F32, 0, 4, false);
type_t* type_f64 = DEFTYPE(TYPE_F64, 0, 8, false);
