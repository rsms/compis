#include "c0lib.h"
#include "compiler.h"

// struct type_t { kind, size, align, isunsigned }

type_t* const type_void = (type_t* const)&(const type_t){ TYPE_VOID };
type_t* const type_bool = (type_t* const)&(const type_t){ TYPE_BOOL, 1, 1, true };

type_t* const type_int  = (type_t* const)&(const type_t){ TYPE_INT, 4, 4 };
type_t* const type_uint = (type_t* const)&(const type_t){ TYPE_INT, 4, 4, true };

type_t* const type_i8  = (type_t* const)&(const type_t){ TYPE_I8,  1, 1 };
type_t* const type_i16 = (type_t* const)&(const type_t){ TYPE_I16, 2, 2 };
type_t* const type_i32 = (type_t* const)&(const type_t){ TYPE_I32, 4, 4 };
type_t* const type_i64 = (type_t* const)&(const type_t){ TYPE_I64, 8, 8 };

type_t* const type_u8  = (type_t* const)&(const type_t){ TYPE_I8,  1, 1, true };
type_t* const type_u16 = (type_t* const)&(const type_t){ TYPE_I16, 2, 2, true };
type_t* const type_u32 = (type_t* const)&(const type_t){ TYPE_I32, 4, 4, true };
type_t* const type_u64 = (type_t* const)&(const type_t){ TYPE_I64, 8, 8, true };

type_t* const type_f32 = (type_t* const)&(const type_t){ TYPE_F32, 4, 4 };
type_t* const type_f64 = (type_t* const)&(const type_t){ TYPE_F64, 8, 8 };
