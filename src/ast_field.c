// SPDX-License-Identifier: Apache-2.0
#include "colib.h"
#include "ast_field.h"


#ifdef DEBUG
  #define FIELD_DEBUGNAME(field_name) .name = #field_name
#else
  #define FIELD_DEBUGNAME(field_name)
#endif

#define FIELD_ID(STRUCT_TYPE, field_name, FIELD_TYPE) \
  { .offs = offsetof(STRUCT_TYPE, field_name), \
    .isid = 1, \
    .type = AST_FIELD_##FIELD_TYPE, \
    FIELD_DEBUGNAME(field_name) \
  }

#define FIELD___(STRUCT_TYPE, field_name, FIELD_TYPE) \
  { .offs = offsetof(STRUCT_TYPE, field_name), \
    .isid = 0, \
    .type = AST_FIELD_##FIELD_TYPE, \
    FIELD_DEBUGNAME(field_name) \
  }

//———————————————————————————————————————————————————————————————————————————————————————
// node & stmt

// per-AST-struct field tables
static const ast_field_t k_fieldsof_node_t[0] = {};
static const ast_field_t k_fieldsof_unit_t[] = {
  FIELD_ID(unit_t, children, NODEARRAY),
  // TODO: srcfile
  // TODO: tfuns
  // TODO: importlist
};
static const ast_field_t k_fieldsof_typedef_t[] = {
  FIELD_ID(typedef_t, type, NODE),
};
static const ast_field_t k_fieldsof_import_t[] = {
  FIELD_ID(import_t, path, STR),
  FIELD___(import_t, pathloc, LOC),
  // TODO: idlist
  // TODO: isfrom
  // TODO: next_import
};
static const ast_field_t k_fieldsof_importid_t[] = {
  // never encoded
};
static const ast_field_t k_fieldsof_templateparam_t[] = {
  FIELD_ID(templateparam_t, name, SYM),
  FIELD_ID(templateparam_t, init, NODEZ),
};

//———————————————————————————————————————————————————————————————————————————————————————
// expr

#define EXPR_FIELDS /* expr_t */ \
  FIELD_ID(expr_t, type, NODEZ) \
// end EXPR_FIELDS
static const ast_field_t k_fieldsof_fun_t[] = {
  EXPR_FIELDS,
  FIELD_ID(fun_t, type,         NODEZ),
  FIELD_ID(fun_t, name,         SYMZ),
  FIELD___(fun_t, nameloc,      LOC),
  FIELD_ID(fun_t, body,         NODEZ),
  FIELD_ID(fun_t, recvt,        NODEZ),
  FIELD___(fun_t, mangledname,  STRZ),
  FIELD___(fun_t, paramsloc,    LOC),
  FIELD___(fun_t, paramsendloc, LOC),
  FIELD___(fun_t, resultloc,    LOC),
  FIELD_ID(fun_t, abi,          U32),
};
static const ast_field_t k_fieldsof_block_t[] = {
  EXPR_FIELDS,
  FIELD_ID(block_t, children, NODEARRAY),
  //TODO: FIELD_ID(block_t, drops, DROPARRAY), // drop_t[]
  FIELD___(block_t, endloc, LOC),
};
static const ast_field_t k_fieldsof_call_t[] = {
  EXPR_FIELDS,
  FIELD_ID(call_t, recv, NODE),
  FIELD_ID(call_t, args, NODEARRAY),
  FIELD___(call_t, argsendloc, LOC),
};
static const ast_field_t k_fieldsof_typecons_t[] = {
  EXPR_FIELDS,
  // TODO
};
static const ast_field_t k_fieldsof_nsexpr_t[] = {
  EXPR_FIELDS,
  FIELD_ID(nsexpr_t, name, SYM),
  FIELD_ID(nsexpr_t, members, NODEARRAY),
};
static const ast_field_t k_fieldsof_idexpr_t[] = {
  EXPR_FIELDS,
  FIELD_ID(idexpr_t, name, SYM),
  FIELD_ID(idexpr_t, ref, NODEZ),
};
static const ast_field_t k_fieldsof_local_t[] = {
  EXPR_FIELDS,
  FIELD_ID(local_t, name,    SYM),
  FIELD___(local_t, nameloc, LOC),
  FIELD_ID(local_t, offset,  U64),
  FIELD_ID(local_t, init,    NODEZ),
};
static const ast_field_t k_fieldsof_member_t[] = { // member_t
  EXPR_FIELDS,
  FIELD_ID(member_t, recv, NODE),
  FIELD_ID(member_t, name, SYM),
  FIELD_ID(member_t, target, NODEZ),
};
static const ast_field_t k_fieldsof_subscript_t[] = { // subscript_t
  EXPR_FIELDS,
  FIELD_ID(subscript_t, recv, NODE),
  FIELD_ID(subscript_t, index, NODE),
  FIELD_ID(subscript_t, index_val, U64),
  FIELD___(subscript_t, endloc, LOC),
};
static const ast_field_t k_fieldsof_unaryop_t[] = {
  EXPR_FIELDS,
  FIELD_ID(unaryop_t, op, U8), // op_t
  FIELD_ID(unaryop_t, expr, NODE),
};
static const ast_field_t k_fieldsof_binop_t[] = {
  EXPR_FIELDS,
  FIELD_ID(binop_t, op, U8), // op_t
  FIELD_ID(binop_t, left, NODE),
  FIELD_ID(binop_t, right, NODE),
};
static const ast_field_t k_fieldsof_ifexpr_t[] = {
  EXPR_FIELDS,
  FIELD_ID(ifexpr_t, cond, NODE),
  FIELD_ID(ifexpr_t, thenb, NODE),
  FIELD_ID(ifexpr_t, elseb, NODEZ),
};
static const ast_field_t k_fieldsof_forexpr_t[] = {
  EXPR_FIELDS,
  FIELD_ID(forexpr_t, start, NODEZ),
  FIELD_ID(forexpr_t, cond, NODE),
  FIELD_ID(forexpr_t, body, NODE),
  FIELD_ID(forexpr_t, end, NODEZ),
};
static const ast_field_t k_fieldsof_retexpr_t[] = {
  EXPR_FIELDS,
  FIELD_ID(retexpr_t, value, NODEZ),
};
static const ast_field_t k_fieldsof_intlit_t[] = {
  EXPR_FIELDS,
  FIELD_ID(intlit_t, intval, U64),
};
static const ast_field_t k_fieldsof_floatlit_t[] = {
  EXPR_FIELDS,
  FIELD_ID(floatlit_t, f64val, F64),
};
static const ast_field_t k_fieldsof_strlit_t[] = {
  EXPR_FIELDS,
  FIELD_ID(strlit_t, bytes, STR),
  FIELD_ID(strlit_t, len,   U64),
};
static const ast_field_t k_fieldsof_arraylit_t[] = {
  EXPR_FIELDS,
  FIELD___(arraylit_t, endloc, LOC),
  FIELD_ID(arraylit_t, values, NODEARRAY),
};

//———————————————————————————————————————————————————————————————————————————————————————
// type

#define TYPE_FIELDS /* type_t */ \
  FIELD_ID(type_t, size,    U64), \
  FIELD_ID(type_t, align,   U8), \
  FIELD___(type_t, _typeid, U64) \
// end TYPE_FIELDS

#define USERTYPE_FIELDS /* usertype_t */ \
  TYPE_FIELDS, \
  FIELD_ID(usertype_t, templateparams, NODEARRAY) \
// end USERTYPE_FIELDS

#define PTRTYPE_FIELDS /* ptrtype_t */ \
  USERTYPE_FIELDS, \
  FIELD_ID(ptrtype_t, elem, NODE) \
// end PTRTYPE_FIELDS


static const ast_field_t k_fieldsof_type_t[] = {
  TYPE_FIELDS,
};
const ast_field_t* g_fieldsof_type_t = k_fieldsof_type_t;
static const ast_field_t k_fieldsof_unresolvedtype_t[] = {
  TYPE_FIELDS,
  FIELD_ID(unresolvedtype_t, name, SYM),
  FIELD_ID(unresolvedtype_t, resolved, NODEZ),
};
static const ast_field_t k_fieldsof_ptrtype_t[] = {
  PTRTYPE_FIELDS,
};
static const ast_field_t k_fieldsof_arraytype_t[] = {
  PTRTYPE_FIELDS,
  FIELD___(arraytype_t, endloc,  LOC),
  FIELD_ID(arraytype_t, len,     U64),
  FIELD_ID(arraytype_t, lenexpr, NODEZ),
};
static const ast_field_t k_fieldsof_reftype_t[] = {
  PTRTYPE_FIELDS,
};
static const ast_field_t k_fieldsof_slicetype_t[] = {
  PTRTYPE_FIELDS,
  FIELD___(slicetype_t, endloc, LOC),
};
static const ast_field_t k_fieldsof_opttype_t[] = {
  PTRTYPE_FIELDS,
};
static const ast_field_t k_fieldsof_funtype_t[] = {
  USERTYPE_FIELDS,
  FIELD_ID(funtype_t, result, NODE),
  FIELD_ID(funtype_t, params, NODEARRAY),
};
static const ast_field_t k_fieldsof_structtype_t[] = {
  USERTYPE_FIELDS,
  FIELD_ID(structtype_t, name,        SYMZ),
  FIELD___(structtype_t, mangledname, STRZ),
  FIELD_ID(structtype_t, fields,      NODEARRAY),
};
static const ast_field_t k_fieldsof_aliastype_t[] = {
  USERTYPE_FIELDS,
  FIELD_ID(aliastype_t, name,        SYM),
  FIELD_ID(aliastype_t, elem,        NODE),
  FIELD___(aliastype_t, mangledname, STRZ),
};
static const ast_field_t k_fieldsof_nstype_t[] = {
  USERTYPE_FIELDS,
  FIELD_ID(nstype_t, members, NODEARRAY),
};
static const ast_field_t k_fieldsof_templatetype_t[] = {
  USERTYPE_FIELDS,
  FIELD_ID(templatetype_t, recv, NODE),
  FIELD_ID(templatetype_t, args, NODEARRAY),
};
static const ast_field_t k_fieldsof_placeholdertype_t[] = {
  USERTYPE_FIELDS,
  FIELD_ID(placeholdertype_t, templateparam, NODE),
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

// nodekind_t => u32 tag
const u32 g_ast_kindtagtab[NODEKIND_COUNT] = {
  #define _(kind, TYPE, tag, ...) [kind] = CO_STRu32(tag),
  FOREACH_NODEKIND(_)
  #undef _
};


// u32 tag => nodekind_t
nodekind_t nodekind_of_tag(u32 tag) {
  switch (tag) {
    #define _(kind, TYPE, tag, ...) \
      case CO_STRu32(tag): return kind;
    FOREACH_NODEKIND(_)
    #undef _
    default: return NODE_BAD;
  }
}


#ifdef DEBUG
UNUSED __attribute__((constructor)) static void check_ast_kindtagtab() {
  for (nodekind_t i = 0; i < (nodekind_t)countof(g_ast_kindtagtab); i++) {
    assertf(g_ast_kindtagtab[i] != 0, "missing g_ast_kindtagtab[%s]", nodekind_name(i));
    for (nodekind_t j = 0; j < (nodekind_t)countof(g_ast_kindtagtab); j++) {
      assertf(i == j || g_ast_kindtagtab[i] != g_ast_kindtagtab[j],
        "duplicate id \"%.4s\": [%s] & [%s]",
        (char*)&g_ast_kindtagtab[i], nodekind_name(i), nodekind_name(j));
    }
  }
}
#endif


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
    //case AST_FIELD_NODELIST:  return "NODELIST";
  }
  return "??";
}
