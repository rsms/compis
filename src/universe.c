#include "c0lib.h"
#include "compiler.h"

// struct type_t { kind, size, align, isunsigned, tid }
#define DEFTYPE(kind, size, isunsigned) \
  (type_t*)&(const type_t){ \
    {kind}, size, size, isunsigned, (char[1]){TYPEID_PREFIX(kind)} \
  }

type_t* type_void = DEFTYPE(TYPE_VOID, 0, false);

const type_t _type_bool = {
  {TYPE_BOOL}, 1, 1, true, (char[1]){TYPEID_PREFIX(TYPE_BOOL)} };
type_t* type_bool = (type_t*)&_type_bool;

type_t* type_int  = DEFTYPE(TYPE_INT, 4, false);
type_t* type_uint = DEFTYPE(TYPE_INT, 4, true);

type_t* type_i8  = DEFTYPE(TYPE_I8,  1, false);
type_t* type_i16 = DEFTYPE(TYPE_I16, 2, false);
type_t* type_i32 = DEFTYPE(TYPE_I32, 4, false);
type_t* type_i64 = DEFTYPE(TYPE_I64, 8, false);

type_t* type_u8  = DEFTYPE(TYPE_I8,  1, true);
type_t* type_u16 = DEFTYPE(TYPE_I16, 2, true);
type_t* type_u32 = DEFTYPE(TYPE_I32, 4, true);
type_t* type_u64 = DEFTYPE(TYPE_I64, 8, true);

type_t* type_f32 = DEFTYPE(TYPE_F32, 4, false);
type_t* type_f64 = DEFTYPE(TYPE_F64, 8, false);

boollit_t* const_true = (boollit_t*)&(const boollit_t){
  {{EXPR_BOOLLIT}}, .type = (type_t*)&_type_bool, .val = true };

boollit_t* const_false = (boollit_t*)&(const boollit_t){
  {{EXPR_BOOLLIT}}, .type = (type_t*)&_type_bool, .val = false };
