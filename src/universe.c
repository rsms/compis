// SPDX-License-Identifier: Apache-2.0
#include "c0lib.h"
#include "compiler.h"

#define DEFTYPE(_kind, _flags, _size) \
  (type_t*)&(const type_t){ \
    .kind = (_kind), \
    .flags = (_flags), \
    .size = (_size), \
    .align = (_size), \
    .tid = (char[2]){TYPEID_PREFIX(_kind),0}, \
  }

type_t* type_void = DEFTYPE(TYPE_VOID, NF_CHECKED, 0);
type_t* type_unknown = DEFTYPE(TYPE_UNKNOWN, NF_CHECKED | NF_UNKNOWN, 0);

type_t* type_bool  = DEFTYPE(TYPE_BOOL, NF_CHECKED, 1);

type_t* type_int  = DEFTYPE(TYPE_INT, NF_CHECKED, 4);
type_t* type_uint = DEFTYPE(TYPE_UINT, NF_CHECKED, 4);

type_t* type_i8  = DEFTYPE(TYPE_I8, NF_CHECKED,  1);
type_t* type_i16 = DEFTYPE(TYPE_I16, NF_CHECKED, 2);
type_t* type_i32 = DEFTYPE(TYPE_I32, NF_CHECKED, 4);
type_t* type_i64 = DEFTYPE(TYPE_I64, NF_CHECKED, 8);

type_t* type_u8  = DEFTYPE(TYPE_U8, NF_CHECKED,  1);
type_t* type_u16 = DEFTYPE(TYPE_U16, NF_CHECKED, 2);
type_t* type_u32 = DEFTYPE(TYPE_U32, NF_CHECKED, 4);
type_t* type_u64 = DEFTYPE(TYPE_U64, NF_CHECKED, 8);

type_t* type_f32 = DEFTYPE(TYPE_F32, NF_CHECKED, 4);
type_t* type_f64 = DEFTYPE(TYPE_F64, NF_CHECKED, 8);
