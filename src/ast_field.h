// AST struct reflection
// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "ast.h"
ASSUME_NONNULL_BEGIN

typedef u8 ast_fieldtype_t;
enum ast_fieldtype {
  AST_FIELD_UNDEF = (ast_fieldtype_t)0,
  AST_FIELD_U8,
  AST_FIELD_U16,
  AST_FIELD_U32,
  AST_FIELD_U64,
  AST_FIELD_F64,
  AST_FIELD_LOC,                   // loc_t
  AST_FIELD_SYM,  AST_FIELD_SYMZ,  // sym_t, sym_t nullable
  AST_FIELD_NODE, AST_FIELD_NODEZ, // node_t*, node_t* nullable
  AST_FIELD_STR,  AST_FIELD_STRZ,  // u8*, u8* nullable
  AST_FIELD_NODEARRAY,             // nodearray_t
  AST_FIELD_NODELIST,              // node_t* nullable
  AST_FIELD_CUSTOM,
};

typedef struct {
  u16             offs;     // offset in struct
  u16             listoffs; // offset of "next" member of list element
  ast_fieldtype_t type;
  #ifdef DEBUG
    const char* name;
  #endif
} ast_field_t;


// g_ast_fieldtab maps nodekind_t => ast_field_t[]
extern const ast_field_t* g_ast_fieldtab[NODEKIND_COUNT];

// g_ast_fieldlentab maps nodekind_t => countof(ast_field_t[])
// i.e. number of fields at corresponding g_ast_fieldtab[kind] entry.
extern const u8 g_ast_fieldlentab[NODEKIND_COUNT];

// g_ast_sizetab maps nodekind_t => sizeof(struct_type)
extern const u8 g_ast_sizetab[NODEKIND_COUNT];

// g_fieldsof_type_t is the fields for type_t, used to test for universal type
extern const ast_field_t* g_fieldsof_type_t;

// ast_fieldtype_str returns a printable name for t, e.g. "U32"
const char* ast_fieldtype_str(ast_fieldtype_t t);


ASSUME_NONNULL_END
