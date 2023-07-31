// SPDX-License-Identifier: Apache-2.0
#include "colib.h"
#include "ast_field.h"


#if DEBUG
  #define FIELD(STRUCT_TYPE, field_name, FIELD_TYPE) \
    { offsetof(STRUCT_TYPE, field_name), 0, \
      AST_FIELD_##FIELD_TYPE, #field_name }
  #define LISTFIELD(STRUCT_TYPE, field_name, ELEM_TYPE, elem_next_field_name) \
    { offsetof(STRUCT_TYPE, field_name), offsetof(ELEM_TYPE, elem_next_field_name),\
      AST_FIELD_NODELIST, #field_name }
#else
  #define FIELD(STRUCT_TYPE, field_name, FIELD_TYPE) \
    { offsetof(STRUCT_TYPE, field_name), 0, AST_FIELD_##FIELD_TYPE }
  #define LISTFIELD(STRUCT_TYPE, field_name, ELEM_TYPE, elem_next_field_name) \
    { offsetof(STRUCT_TYPE, field_name), offsetof(ELEM_TYPE, elem_next_field_name),\
      AST_FIELD_NODELIST }
#endif


// per-AST-struct field tables
static const ast_field_t k_fieldsof_node_t[0] = {};
static const ast_field_t k_fieldsof_unit_t[] = {
  FIELD(unit_t, children, NODEARRAY),
  // TODO: srcfile
  // TODO: tfuns
  // TODO: importlist
};
static const ast_field_t k_fieldsof_typedef_t[] = {
  FIELD(typedef_t, type, NODE),
};
static const ast_field_t k_fieldsof_import_t[] = {
  FIELD(import_t, path, STR),
  FIELD(import_t, pathloc, LOC),
  // TODO: idlist
  // TODO: isfrom
  // TODO: next_import
};
static const ast_field_t k_fieldsof_importid_t[] = {
  // never encoded
};
static const ast_field_t k_fieldsof_templateparam_t[] = {
  FIELD(templateparam_t, name, SYM),
  FIELD(templateparam_t, init, NODEZ),
};


#define EXPR_FIELDS /* expr_t */ \
  FIELD(expr_t, type, NODEZ) \
// end EXPR_FIELDS
static const ast_field_t k_fieldsof_fun_t[] = {
  EXPR_FIELDS,
  FIELD(fun_t, type,         NODEZ),
  FIELD(fun_t, name,         SYMZ),
  FIELD(fun_t, nameloc,      LOC),
  FIELD(fun_t, body,         NODEZ),
  FIELD(fun_t, recvt,        NODEZ),
  FIELD(fun_t, mangledname,  STRZ),
  FIELD(fun_t, paramsloc,    LOC),
  FIELD(fun_t, paramsendloc, LOC),
  FIELD(fun_t, resultloc,    LOC),
  FIELD(fun_t, abi,          U32),
};
static const ast_field_t k_fieldsof_block_t[] = {
  EXPR_FIELDS,
  FIELD(block_t, children, NODEARRAY),
  //TODO: FIELD(block_t, drops, DROPARRAY), // drop_t[]
  FIELD(block_t, endloc, LOC),
};
static const ast_field_t k_fieldsof_call_t[] = {
  EXPR_FIELDS,
  FIELD(call_t, recv, NODE),
  FIELD(call_t, args, NODEARRAY),
  FIELD(call_t, argsendloc, LOC),
};
static const ast_field_t k_fieldsof_typecons_t[] = {
  EXPR_FIELDS,
  // TODO
};
static const ast_field_t k_fieldsof_nsexpr_t[] = {
  EXPR_FIELDS,
  FIELD(nsexpr_t, name, SYM),
  FIELD(nsexpr_t, members, NODEARRAY),
};
static const ast_field_t k_fieldsof_idexpr_t[] = {
  EXPR_FIELDS,
  FIELD(idexpr_t, name, SYM),
  FIELD(idexpr_t, ref, NODEZ),
};
static const ast_field_t k_fieldsof_local_t[] = {
  EXPR_FIELDS,
  FIELD(local_t, name,    SYM),
  FIELD(local_t, nameloc, LOC),
  FIELD(local_t, offset,  U64),
  FIELD(local_t, init,    NODEZ),
};
static const ast_field_t k_fieldsof_member_t[] = { // member_t
  EXPR_FIELDS,
  FIELD(member_t, recv, NODE),
  FIELD(member_t, name, SYM),
  FIELD(member_t, target, NODEZ),
};
static const ast_field_t k_fieldsof_subscript_t[] = { // subscript_t
  EXPR_FIELDS,
  FIELD(subscript_t, recv, NODE),
  FIELD(subscript_t, index, NODE),
  FIELD(subscript_t, index_val, U64),
  FIELD(subscript_t, endloc, LOC),
};
static const ast_field_t k_fieldsof_unaryop_t[] = {
  EXPR_FIELDS,
  FIELD(unaryop_t, op, U8), // op_t
  FIELD(unaryop_t, expr, NODE),
};
static const ast_field_t k_fieldsof_binop_t[] = {
  EXPR_FIELDS,
  FIELD(binop_t, op, U8), // op_t
  FIELD(binop_t, left, NODE),
  FIELD(binop_t, right, NODE),
};
static const ast_field_t k_fieldsof_ifexpr_t[] = {
  EXPR_FIELDS,
  FIELD(ifexpr_t, cond, NODE),
  FIELD(ifexpr_t, thenb, NODE),
  FIELD(ifexpr_t, elseb, NODEZ),
};
static const ast_field_t k_fieldsof_forexpr_t[] = {
  EXPR_FIELDS,
  FIELD(forexpr_t, start, NODEZ),
  FIELD(forexpr_t, cond, NODE),
  FIELD(forexpr_t, body, NODE),
  FIELD(forexpr_t, end, NODEZ),
};
static const ast_field_t k_fieldsof_retexpr_t[] = {
  EXPR_FIELDS,
  FIELD(retexpr_t, value, NODEZ),
};
static const ast_field_t k_fieldsof_intlit_t[] = {
  EXPR_FIELDS,
  FIELD(intlit_t, intval, U64),
};
static const ast_field_t k_fieldsof_floatlit_t[] = {
  EXPR_FIELDS,
  FIELD(floatlit_t, f64val, F64),
};
static const ast_field_t k_fieldsof_strlit_t[] = {
  EXPR_FIELDS,
  FIELD(strlit_t, bytes, STR),
  FIELD(strlit_t, len,   U64),
};
static const ast_field_t k_fieldsof_arraylit_t[] = {
  EXPR_FIELDS,
  FIELD(arraylit_t, endloc, LOC),
  FIELD(arraylit_t, values, NODEARRAY),
};



#define TYPE_FIELDS /* type_t */ \
  FIELD(type_t, size,  U64), \
  FIELD(type_t, align, U8), \
  FIELD(type_t, tid,   SYMZ) \
// end TYPE_FIELDS

#define USERTYPE_FIELDS /* usertype_t */ \
  TYPE_FIELDS, \
  LISTFIELD(usertype_t, templateparams, templateparam_t, next_templateparam) \
// end USERTYPE_FIELDS

#define PTRTYPE_FIELDS /* ptrtype_t */ \
  USERTYPE_FIELDS, \
  FIELD(ptrtype_t, elem, NODE) \
// end PTRTYPE_FIELDS

static const ast_field_t k_fieldsof_type_t[] = {
  TYPE_FIELDS,
};
const ast_field_t* g_fieldsof_type_t = k_fieldsof_type_t;
static const ast_field_t k_fieldsof_unresolvedtype_t[] = {
  TYPE_FIELDS,
  FIELD(unresolvedtype_t, name, SYM),
  FIELD(unresolvedtype_t, resolved, NODEZ),
};
static const ast_field_t k_fieldsof_ptrtype_t[] = {
  PTRTYPE_FIELDS,
};
static const ast_field_t k_fieldsof_arraytype_t[] = {
  PTRTYPE_FIELDS,
  FIELD(arraytype_t, endloc,  LOC),
  FIELD(arraytype_t, len,     U64),
  FIELD(arraytype_t, lenexpr, NODEZ),
};
static const ast_field_t k_fieldsof_reftype_t[] = {
  PTRTYPE_FIELDS,
};
static const ast_field_t k_fieldsof_slicetype_t[] = {
  PTRTYPE_FIELDS,
  FIELD(slicetype_t, endloc, LOC),
};
static const ast_field_t k_fieldsof_opttype_t[] = {
  PTRTYPE_FIELDS,
};
static const ast_field_t k_fieldsof_funtype_t[] = {
  USERTYPE_FIELDS,
  FIELD(funtype_t, result, NODE),
  FIELD(funtype_t, params, NODEARRAY),
};
static const ast_field_t k_fieldsof_structtype_t[] = {
  USERTYPE_FIELDS,
  FIELD(structtype_t, name,        SYMZ),
  FIELD(structtype_t, mangledname, STRZ),
  FIELD(structtype_t, fields,      NODEARRAY),
};
static const ast_field_t k_fieldsof_aliastype_t[] = {
  USERTYPE_FIELDS,
  FIELD(aliastype_t, name,        SYM),
  FIELD(aliastype_t, elem,        NODE),
  FIELD(aliastype_t, mangledname, STRZ),
};
static const ast_field_t k_fieldsof_nstype_t[] = {
  USERTYPE_FIELDS,
  FIELD(nstype_t, members, NODEARRAY),
};
static const ast_field_t k_fieldsof_templatetype_t[] = {
  USERTYPE_FIELDS,
  FIELD(templatetype_t, recv, NODE),
  FIELD(templatetype_t, args, NODEARRAY),
};
static const ast_field_t k_fieldsof_placeholdertype_t[] = {
  USERTYPE_FIELDS,
  FIELD(placeholdertype_t, templateparam, NODE),
};

#undef FIELD
#undef EXPR_FIELDS
#undef TYPE_FIELDS
#undef PTRTYPE_FIELDS

// g_ast_fieldtab maps nodekind_t => ast_field_t[]
const ast_field_t* g_ast_fieldtab[NODEKIND_COUNT] = {
  #define _(kind, TYPE, tag, ...) [kind] = k_fieldsof_##TYPE,
  FOREACH_NODEKIND(_)
  #undef _
};

// g_ast_fieldlentab maps nodekind_t => countof(ast_field_t[])
const u8 g_ast_fieldlentab[NODEKIND_COUNT] = {
  #define _(kind, TYPE, tag, ...) [kind] = countof(k_fieldsof_##TYPE),
  FOREACH_NODEKIND(_)
  #undef _
};

// nodekind_t => sizeof(struct_type)
const u8 g_ast_sizetab[NODEKIND_COUNT] = {
  #define _(kind, TYPE, tag, ...) [kind] = sizeof(TYPE),
  FOREACH_NODEKIND(_)
  #undef _
};


const char* ast_fieldtype_str(ast_fieldtype_t t) {
  switch ((enum ast_fieldtype)t) {
    case AST_FIELD_UNDEF:     return "UNDEF";
    case AST_FIELD_U8:        return "U8";
    case AST_FIELD_U16:       return "U16";
    case AST_FIELD_U32:       return "U32";
    case AST_FIELD_U64:       return "U64";
    case AST_FIELD_F64:       return "F64";
    case AST_FIELD_LOC:       return "LOC";
    case AST_FIELD_SYM:       return "SYM";
    case AST_FIELD_SYMZ:      return "SYMZ";
    case AST_FIELD_NODE:      return "NODE";
    case AST_FIELD_NODEZ:     return "NODEZ";
    case AST_FIELD_STR:       return "STR";
    case AST_FIELD_STRZ:      return "STRZ";
    case AST_FIELD_NODEARRAY: return "NODEARRAY";
    case AST_FIELD_NODELIST:  return "NODELIST";
    case AST_FIELD_CUSTOM:    return "CUSTOM";
  }
  return "??";
}
