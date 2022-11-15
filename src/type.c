#include "c0lib.h"
#include "compiler.h"

// struct type_t { kind, size, align, isunsigned, tid }

#define DEF(kind, size, isunsigned) \
  (type_t*)&(const type_t){ \
    {kind}, size, size, isunsigned, (char[1]){TYPEID_PREFIX(kind)} \
  }

type_t* type_void = DEF(TYPE_VOID, 0, false);
type_t* type_bool = DEF(TYPE_BOOL, 1, true);

type_t* type_int  = DEF(TYPE_INT, 4, false);
type_t* type_uint = DEF(TYPE_INT, 4, true);

type_t* type_i8  = DEF(TYPE_I8,  1, false);
type_t* type_i16 = DEF(TYPE_I16, 2, false);
type_t* type_i32 = DEF(TYPE_I32, 4, false);
type_t* type_i64 = DEF(TYPE_I64, 8, false);

type_t* type_u8  = DEF(TYPE_I8,  1, true);
type_t* type_u16 = DEF(TYPE_I16, 2, true);
type_t* type_u32 = DEF(TYPE_I32, 4, true);
type_t* type_u64 = DEF(TYPE_I64, 8, true);

type_t* type_f32 = DEF(TYPE_F32, 4, false);
type_t* type_f64 = DEF(TYPE_F64, 8, false);
