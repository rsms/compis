// SPDX-License-Identifier: Apache-2.0
#pragma once
ASSUME_NONNULL_BEGIN

typedef const char* sym_t;

extern sym_t sym__;       // "_"
extern sym_t sym_this;    // "this"
extern sym_t sym_drop;    // "drop"
extern sym_t sym_main;    // "main"
extern sym_t sym_str;     // "str"
extern sym_t sym_as;      // "as"
extern sym_t sym_from;    // "from"
extern sym_t sym_len;     // "len"
extern sym_t sym_cap;     // "cap"
extern sym_t sym_reserve; // "reserve"
extern sym_t sym_resize;  // "resize"

extern sym_t sym_void;    // "void"
extern sym_t sym_bool;    // "bool"
extern sym_t sym_int;     // "int"
extern sym_t sym_uint;    // "uint"
extern sym_t sym_i8;      // "i8"
extern sym_t sym_i16;     // "i16"
extern sym_t sym_i32;     // "i32"
extern sym_t sym_i64;     // "i64"
extern sym_t sym_u8;      // "u8"
extern sym_t sym_u16;     // "u16"
extern sym_t sym_u32;     // "u32"
extern sym_t sym_u64;     // "u64"
extern sym_t sym_f32;     // "f32"
extern sym_t sym_f64;     // "f64"
extern sym_t sym_unknown; // "unknown"

extern sym_t sym___inc__; // "__inc__"
extern sym_t sym___dec__; // "__dec__"
extern sym_t sym___inv__; // "__inv__"
extern sym_t sym___not__; // "__not__"
extern sym_t sym___add__; // "__add__"
extern sym_t sym___sub__; // "__sub__"
extern sym_t sym___mul__; // "__mul__"
extern sym_t sym___div__; // "__div__"
extern sym_t sym___mod__; // "__mod__"
extern sym_t sym___and__; // "__and__"
extern sym_t sym___or__; // "__or__"
extern sym_t sym___xor__; // "__xor__"
extern sym_t sym___shl__; // "__shl__"
extern sym_t sym___shr__; // "__shr__"
extern sym_t sym___land__; // "__land__"
extern sym_t sym___lor__; // "__lor__"
extern sym_t sym___eq__; // "__eq__"
extern sym_t sym___neq__; // "__neq__"
extern sym_t sym___lt__; // "__lt__"
extern sym_t sym___gt__; // "__gt__"
extern sym_t sym___lteq__; // "__lteq__"
extern sym_t sym___gteq__; // "__gteq__"

void sym_init(memalloc_t);
sym_t sym_intern(const char* key, usize keylen);
inline static sym_t sym_cstr(const char* s) { return sym_intern(s, strlen(s)); }
sym_t sym_snprintf(char* buf, usize bufcap, const char* fmt, ...)ATTR_FORMAT(printf,3,4);


ASSUME_NONNULL_END
