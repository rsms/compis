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

extern sym_t sym_void;    // extern sym_t sym_void_typeid;
extern sym_t sym_bool;    // extern sym_t sym_bool_typeid;
extern sym_t sym_int;     // extern sym_t sym_int_typeid;
extern sym_t sym_uint;    // extern sym_t sym_uint_typeid;
extern sym_t sym_i8;      // extern sym_t sym_i8_typeid;
extern sym_t sym_i16;     // extern sym_t sym_i16_typeid;
extern sym_t sym_i32;     // extern sym_t sym_i32_typeid;
extern sym_t sym_i64;     // extern sym_t sym_i64_typeid;
extern sym_t sym_u8;      // extern sym_t sym_u8_typeid;
extern sym_t sym_u16;     // extern sym_t sym_u16_typeid;
extern sym_t sym_u32;     // extern sym_t sym_u32_typeid;
extern sym_t sym_u64;     // extern sym_t sym_u64_typeid;
extern sym_t sym_f32;     // extern sym_t sym_f32_typeid;
extern sym_t sym_f64;     // extern sym_t sym_f64_typeid;
extern sym_t sym_unknown; // extern sym_t sym_unknown_typeid;

void sym_init(memalloc_t);
sym_t sym_intern(const char* key, usize keylen);
inline static sym_t sym_cstr(const char* s) { return sym_intern(s, strlen(s)); }
sym_t sym_snprintf(char* buf, usize bufcap, const char* fmt, ...)ATTR_FORMAT(printf,3,4);


ASSUME_NONNULL_END
