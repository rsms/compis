// SPDX-License-Identifier: Apache-2.0
#include "colib.h"
#include "ast.h"
#include "ast_field.h"

// node kind string table with compressed indices (compared to table of pointers.)
// We end up with something like this; one string with indices:
//   enum {
//     NK_NBAD, NK__NBAD = NK_NBAD + strlen("NBAD"),
//     NK_NCOMMENT, NK__NCOMMENT = NK_NCOMMENT + strlen("NCOMMENT"),
//     NK_NUNIT, NK__NUNIT = NK_NUNIT + strlen("NUNIT"),
//     NK_NFUN, NK__NFUN = NK_NFUN + strlen("NFUN"),
//     NK_NBLOCK, NK__NBLOCK = NK_NBLOCK + strlen("NBLOCK"),
//     NK_UNKNOWN, NK__UNKNOWN = NK_UNKNOWN + strlen("N?"),
//   };
//   static const struct { u8 offs[NODEKIND_COUNT]; char strs[]; } nodekind_strtab = {
//     { NK_NBAD, NK_NCOMMENT, NK_NUNIT, NK_NFUN, NK_NBLOCK },
//     { "NBAD\0NCOMMENT\0NUNIT\0NFUN\0NBLOCK\0N?" }
//   };
#define NK_UNKNOWN_STR "NODE_???"
enum {
  #define _(NAME, ...) NK_##NAME, NK__##NAME = NK_##NAME + strlen(#NAME),
  FOREACH_NODEKIND(_)
  #undef _
  NK_UNKNOWN, NK__UNKNOWN = NK_UNKNOWN + strlen(NK_UNKNOWN_STR),
};
static const struct {
  int  offs[NODEKIND_COUNT + 1]; // index into strs
  char strs[];
} nodekind_strtab = {
  { // get offset from enum
    #define _(NAME, ...) NK_##NAME,
    FOREACH_NODEKIND(_)
    #undef _
    NK_UNKNOWN,
  }, {
    #define _(NAME, ...) #NAME "\0"
    FOREACH_NODEKIND(_)
    #undef _
    NK_UNKNOWN_STR
  }
};


#define STRTAB_GET(strtab, kind) \
  &(strtab).strs[ (strtab).offs[ MIN((kind), countof(strtab.offs)-1) ] ]


const char* nodekind_name(nodekind_t kind) {
  return STRTAB_GET(nodekind_strtab, kind);
}


const char* node_srcfilename(const node_t* n, locmap_t* lm) {
  const srcfile_t* sf = n->loc != 0 ? loc_srcfile(n->loc, lm) : NULL;
  return sf ? sf->name.p : "<input>";
}


bool ast_is_main_fun(const fun_t* fn) {
  return
    fn->kind == EXPR_FUN &&
    fn->recvt == NULL &&
    fn->name == sym_main &&
    (fn->flags & NF_VIS_PUB) &&
    (fn->nsparent != NULL && fn->nsparent->kind == NODE_UNIT);
}


//———————————————————————————————————————————————————————————————————————————————————————
// ast_clone_node


node_t* nullable _ast_clone_node(memalloc_t ma, const void* np) {
  const node_t* n = np;
  usize nodesize = g_ast_sizetab[n->kind];
  node_t* n2 = mem_alloc(ma, nodesize).p;
  if UNLIKELY(n2 == NULL)
    return NULL;

  // copy field values
  memcpy(n2, n, nodesize);

  // copy array data
  const ast_field_t* fieldtab = g_ast_fieldtab[n2->kind];
  u8 fieldlen = g_ast_fieldlentab[n2->kind];
  for (u8 fieldidx = 0; fieldidx < fieldlen; fieldidx++) {
    ast_field_t f = fieldtab[fieldidx];
    if (f.type == AST_FIELD_NODEARRAY) {
      nodearray_t* na = (void*)n2 + f.offs;
      void* p = mem_alloc(ma, na->len * sizeof(node_t*)).p;
      if (!p)
        return NULL;
      memcpy(p, na->v, na->len * sizeof(node_t*));
      na->v = p;
      na->cap = na->len;
    }
  }

  return n2;
}


//———————————————————————————————————————————————————————————————————————————————————————
// ast_childit

typedef struct {
  node_t*            n;
  const ast_field_t* fieldtab;
  u8                 fieldlen;
  u8                 fieldidx;
  #ifdef DEBUG
  u8                 isconst;
  #endif
  u32                arrayidx;
} ast_childit_impl_t;

static_assert(sizeof(ast_childit_t) == sizeof(ast_childit_impl_t), "");


ast_childit_t ast_childit(node_t* n) {
  ast_childit_impl_t it = {
    .n = n,
    .fieldtab = g_ast_fieldtab[n->kind],
    .fieldlen = g_ast_fieldlentab[n->kind],
    .fieldidx = 0,
  };
  return *(ast_childit_t*)&it;
}


ast_childit_t ast_childit_const(const node_t* n) {
  ast_childit_t it = ast_childit((node_t*)n);
  #ifdef DEBUG
  ((ast_childit_impl_t*)&it)->isconst = 1;
  #endif
  return it;
}


const node_t* nullable ast_childit_const_next(ast_childit_t* itp) {
  assert(((ast_childit_impl_t*)itp)->isconst);
  node_t** np = ast_childit_next(itp);
  return np ? (node_t*)*np : NULL;
}


node_t** nullable ast_childit_next(ast_childit_t* itp) {
  ast_childit_impl_t* it = (ast_childit_impl_t*)itp;

  while (it->fieldidx < it->fieldlen) {
    ast_field_t f = it->fieldtab[it->fieldidx];
    void* fp = (void*)it->n + f.offs;

    // dlog("** %s %s", f.name, ast_fieldtype_str(f.type));

    switch ((enum ast_fieldtype)f.type) {

    case AST_FIELD_NODEZ:
      if (*(void**)fp == NULL)
        break;
      FALLTHROUGH;
    case AST_FIELD_NODE:
      it->fieldidx++;
      return (node_t**)fp;

    case AST_FIELD_NODEARRAY:
      if (it->arrayidx < ((nodearray_t*)fp)->len)
        return &((nodearray_t*)fp)->v[it->arrayidx++];
      it->arrayidx = 0;
      break;

    // no nodes
    case AST_FIELD_U8:
    case AST_FIELD_U16:
    case AST_FIELD_U32:
    case AST_FIELD_U64:
    case AST_FIELD_F64:
    case AST_FIELD_LOC:
    case AST_FIELD_SYM:
    case AST_FIELD_SYMZ:
    case AST_FIELD_STR:
    case AST_FIELD_STRZ:
    case AST_FIELD_UNDEF:
      break;

    } // switch

    it->fieldidx++;
  }
  return NULL;
}


//———————————————————————————————————————————————————————————————————————————————————————
// ast_transform


typedef struct ast_transform_ {
  ast_transformer_t trfn;
  memalloc_t        ma;
  memalloc_t        ast_ma;
  nodearray_t       seenstack;
  err_t             err;
} ast_transform_t;


static bool ast_transform_clone(ast_transform_t* tr, node_t** np) {
  node_t* n2 = ast_clone_node(tr->ast_ma, *np);
  //dlog("%s: %s %p -> %p", __FUNCTION__, nodekind_name((*np)->kind), *np, n2);
  if UNLIKELY(!n2 || !nodearray_push(&tr->seenstack, tr->ma, n2)) {
    tr->err = ErrNoMem;
    return false;
  }
  *np = n2;
  return true;
}


static void* nullable ast_transform_child(ast_transform_t* tr, node_t* n, void* ctx) {
  if (tr->err)
    return n;
  for (u32 i = tr->seenstack.len; i > 0;) {
    if (tr->seenstack.v[--i] == n)
      return n;
  }
  if UNLIKELY(!nodearray_push(&tr->seenstack, tr->ma, n)) {
    tr->err = ErrNoMem;
    return n;
  }
  //dlog("%s: %s", __FUNCTION__, nodekind_name(n->kind));
  n = tr->trfn(tr, n, ctx);
  tr->seenstack.len--; // pop
  return n;
}


node_t* nullable ast_transform_children(
  ast_transform_t* tr, node_t* n, void* nullable ctx)
{
  if (tr->err)
    return n;

  // initial node is top of stack; it was cloned if n is different
  assert(tr->seenstack.len > 0);
  bool is_clone = n != tr->seenstack.v[tr->seenstack.len - 1];

  // if n was replaced, register it in seenstack
  if (is_clone) {
    if UNLIKELY(!nodearray_push(&tr->seenstack, tr->ma, n)) {
      tr->err = ErrNoMem;
      return n;
    }
  }

  // visit expression's type
  if (node_isexpr(n) && ((expr_t*)n)->type) {
    expr_t* expr = (expr_t*)n;
    type_t* type2 = ast_transform_child(tr, (node_t*)expr->type, ctx);
    if (type2 != expr->type) {
      if UNLIKELY(!is_clone && !ast_transform_clone(tr, &n))
        return n;
      ((expr_t*)n)->type = type2;
    }
  }

  // visit fields
  const ast_field_t* fieldtab = g_ast_fieldtab[n->kind];
  u8 fieldlen = g_ast_fieldlentab[n->kind];

  for (u8 fieldidx = 0; fieldidx < fieldlen; fieldidx++) {
    ast_field_t f = fieldtab[fieldidx];
    void* fp = (void*)n + f.offs;

    switch ((enum ast_fieldtype)f.type) {

    case AST_FIELD_NODEZ:
      if (*(void**)fp == NULL)
        break;
      FALLTHROUGH;
    case AST_FIELD_NODE: {
      node_t* cn = *(node_t**)fp;
      node_t* cn2 = ast_transform_child(tr, cn, ctx);
      if (cn2 == cn)
        break;
      if UNLIKELY(!is_clone) {
        if (!ast_transform_clone(tr, &n))
          return n;
        fp = (void*)n + f.offs; // load new field pointer
      }
      *(node_t**)fp = cn2;
      break;
    }

    case AST_FIELD_NODEARRAY: {
      nodearray_t* na = fp;
      for (u32 i = 0, end = na->len; i < end; i++) {
        node_t* cn = na->v[i];
        node_t* cn2 = ast_transform_child(tr, cn, ctx);
        if (cn == cn2)
          continue;
        if UNLIKELY(!is_clone) {
          if (!ast_transform_clone(tr, &n))
            return n;
          fp = (void*)n + f.offs; // load new field pointer
          na = fp; // load new nodearray
        }
        na->v[i] = cn2;
      }
      break;
    }

    case AST_FIELD_U8:
    case AST_FIELD_U16:
    case AST_FIELD_U32:
    case AST_FIELD_U64:
    case AST_FIELD_F64:
    case AST_FIELD_LOC:
    case AST_FIELD_SYM:
    case AST_FIELD_SYMZ:
    case AST_FIELD_STR:
    case AST_FIELD_STRZ:
    case AST_FIELD_UNDEF:
      break;

    } // switch
  }

  if (is_clone)
    tr->seenstack.len--; // pop

  return n;
}


err_t ast_transform(
  node_t*           n,
  memalloc_t        ast_ma,
  ast_transformer_t trfn,
  void* nullable    ctx,
  node_t**          result)
{
  ast_transform_t tr = {
    .trfn = trfn,
    .ma = memalloc_ctx(),
    .ast_ma = ast_ma,
  };
  if UNLIKELY(!nodearray_push(&tr.seenstack, tr.ma, n))
    return ErrNoMem;
  *result = trfn(&tr, n, ctx);
  nodearray_dispose(&tr.seenstack, tr.ma);
  return tr.err;
}


//———————————————————————————————————————————————————————————————————————————————————————


bool ast_toposort_visit_def(
  nodearray_t* defs, memalloc_t ma, nodeflag_t visibility, node_t* n)
{
  switch (n->kind) {

    case EXPR_FUN:
      if (visibility && (n->flags & visibility) == 0)
        return true;
      FALLTHROUGH;
    case TYPE_ARRAY:
    case TYPE_FUN:
    case TYPE_PTR:
    case TYPE_REF:
    case TYPE_MUTREF:
    case TYPE_SLICE:
    case TYPE_MUTSLICE:
    case TYPE_OPTIONAL:
    case TYPE_ALIAS:
    case TYPE_STRUCT:
    case TYPE_NS:
    case TYPE_TEMPLATE:
      //dlog("[%s] %s#%p", __FUNCTION__, nodekind_name(n->kind), n);
      // If MARK1 is set, n is currently being visited (recursive)
      if UNLIKELY(n->flags & NF_MARK1) {
        // insert a "forward declaration" node for the recursive definition
        fwddecl_t* fwddecl = mem_alloct(ma, fwddecl_t);
        if (!fwddecl)
          return false;
        fwddecl->kind = NODE_FWDDECL;
        fwddecl->decl = n;
        if (!nodearray_push(defs, ma, (node_t*)fwddecl))
          return false;
        return true;
      }
      // stop now if n has been visited already
      for (u32 i = defs->len; i > 0; ) {
        if (defs->v[--i] == n)
          return true;
      }
      // mark n as "currently being visited"
      n->flags |= NF_MARK1;
      break;

    case TYPE_PLACEHOLDER: {
      // treat placeholdertype_t specially to avoid adding it to defs.
      // note: don't store to n to avoid tripping msan when init is null.
      node_t* init = assertnotnull(((placeholdertype_t*)n)->templateparam)->init;
      if (init)
        MUSTTAIL return ast_toposort_visit_def(defs, ma, visibility, init);
      return true;
    }

    default:
      break;
  }

  // visit children
  ast_childit_t it = ast_childit(n);
  for (node_t** cnp; (cnp = ast_childit_next(&it));) {
    if (!ast_toposort_visit_def(defs, ma, visibility, *cnp))
      return false;
  }

  // if node may have had children that were visited
  if (n->flags & NF_MARK1) {
    // clear "currently being visited" marker
    n->flags &= ~NF_MARK1;
    // mark node as "has been visited" by adding it to the defs array
    if (!nodearray_push(defs, ma, (node_t*)n))
      return false;
  }

  return true;
}
