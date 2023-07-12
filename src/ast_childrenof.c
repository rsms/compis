// SPDX-License-Identifier: Apache-2.0
#include "colib.h"
#include "compiler.h"


static bool add_nodes(nodearray_t* children, memalloc_t ma, nodearray_t na) {
  node_t** p = nodearray_alloc(children, ma, na.len);
  if (!p)
    return false;
  memcpy(p, na.v, na.len * sizeof(void*));

  #ifdef DEBUG
  for (u32 i = 0; i < na.len; i++) {
    // dlog("%s na.v[%u] %s", __FUNCTION__, i, nodekind_name(((node_t*)na.v[i])->kind));
    assertf(na.v[i] != NULL, "nodes[%u]", i);
  }
  #endif

  return true;
}


err_t ast_childrenof(nodearray_t* children, memalloc_t ma, const node_t* np) {
  #define ADD_NODE(n) ({ \
    if ((n) && !nodearray_push(children, ma, (void*)(n))) \
      goto errnomem; \
  })

  #define ADD_ARRAY_OF_NODES(pa) \
    ({ if (!add_nodes(children, ma, *(pa))) \
         goto errnomem; })

  if (node_isexpr(np) && (np->kind < EXPR_BOOLLIT || EXPR_STRLIT < np->kind))
    ADD_NODE(((const expr_t*)np)->type);

  switch ((enum nodekind)np->kind) {
  // no children
  case NODE_BAD:
  case NODE_COMMENT:
  case STMT_IMPORT: // import_t
  case EXPR_BOOLLIT:
  case EXPR_INTLIT:
  case EXPR_FLOATLIT:
  case EXPR_STRLIT:
  case TYPE_VOID:
  case TYPE_BOOL:
  case TYPE_I8:
  case TYPE_I16:
  case TYPE_I32:
  case TYPE_I64:
  case TYPE_INT:
  case TYPE_U8:
  case TYPE_U16:
  case TYPE_U32:
  case TYPE_U64:
  case TYPE_UINT:
  case TYPE_F32:
  case TYPE_F64:
  case TYPE_UNKNOWN:
    break;

  case NODE_UNIT: { const unit_t* n = (unit_t*)np;
    ADD_ARRAY_OF_NODES(&n->children); break; }

  case STMT_TYPEDEF: { const typedef_t* n = (typedef_t*)np;
    ADD_NODE(n->type); break; }

  case EXPR_ARRAYLIT: { const arraylit_t* n = (arraylit_t*)np;
    ADD_ARRAY_OF_NODES(&n->values); break; }

  case EXPR_BLOCK: { const block_t* n = (block_t*)np;
    ADD_ARRAY_OF_NODES(&n->children); break; }

  case EXPR_ASSIGN:
  case EXPR_BINOP: { const binop_t* n = (binop_t*)np;
    ADD_NODE(n->left);
    ADD_NODE(n->right); break; }

  case EXPR_DEREF:
  case EXPR_POSTFIXOP:
  case EXPR_PREFIXOP: { const unaryop_t* n = (unaryop_t*)np;
    ADD_NODE(n->expr); break; }

  case EXPR_ID: { const idexpr_t* n = (idexpr_t*)np;
    ADD_NODE(n->ref); break; }

  case EXPR_RETURN: { const retexpr_t* n = (retexpr_t*)np;
    ADD_NODE(n->value); break; }

  case EXPR_VAR:
  case EXPR_LET:
  case EXPR_PARAM:
  case EXPR_FIELD: { const local_t* n = (local_t*)np;
    ADD_NODE(n->init); break; }

  case EXPR_CALL: { const call_t* n = (call_t*)np;
    ADD_NODE(n->recv);
    ADD_ARRAY_OF_NODES(&n->args); break; }

  case EXPR_IF: { const ifexpr_t* n = (ifexpr_t*)np;
    ADD_NODE(n->cond);
    ADD_NODE(n->thenb);
    ADD_NODE(n->elseb); break; }

  case EXPR_FOR: { const forexpr_t* n = (forexpr_t*)np;
    ADD_NODE(n->start);
    ADD_NODE(n->cond);
    ADD_NODE(n->body);
    ADD_NODE(n->end); break; }

  case EXPR_FUN: { const fun_t* n = (fun_t*)np;
    ADD_NODE(n->recvt);
    ADD_NODE(n->body); break; }

  case EXPR_MEMBER: { const member_t* n = (member_t*)np;
    ADD_NODE(n->recv);
    ADD_NODE(n->target); break; }

  case EXPR_SUBSCRIPT: { const subscript_t* n = (subscript_t*)np;
    ADD_NODE(n->recv);
    ADD_NODE(n->index); break; }

  case EXPR_TYPECONS: { const typecons_t* n = (typecons_t*)np;
    if (n->type) {
      if (type_isprim(n->type)) {
        ADD_NODE(n->expr);
      } else {
        ADD_ARRAY_OF_NODES(&n->args);
      }
    }
    break; }



  case TYPE_ALIAS: { const aliastype_t* n = (aliastype_t*)np;
    ADD_NODE(n->elem); break; }

  case TYPE_ARRAY: { const arraytype_t* n = (arraytype_t*)np;
    ADD_NODE(n->lenexpr); break; }

  case TYPE_FUN: { const funtype_t* n = (funtype_t*)np;
    ADD_ARRAY_OF_NODES(&n->params);
    ADD_NODE(n->result); break; }

  case TYPE_PTR:
  case TYPE_REF:
  case TYPE_MUTREF:
  case TYPE_OPTIONAL:
  case TYPE_SLICE:
  case TYPE_MUTSLICE:
    { const ptrtype_t* n = (ptrtype_t*)np;
    ADD_NODE(n->elem); break; }

  case TYPE_STRUCT: { const structtype_t* n = (structtype_t*)np;
    ADD_ARRAY_OF_NODES(&n->fields); break; }

  case TYPE_UNRESOLVED: { const unresolvedtype_t* n = (unresolvedtype_t*)np;
    ADD_NODE(n->resolved); break; }

  }

  return 0;
errnomem:
  return ErrNoMem;
}
