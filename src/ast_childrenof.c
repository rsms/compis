// SPDX-License-Identifier: Apache-2.0
#include "colib.h"
#include "compiler.h"


typedef struct astiterx_ astiterx_t;

typedef const node_t* nullable(*itfun_t)(astiterx_t*);

typedef struct astiterx_ {
  itfun_t f;
  union {
    u64 state[2];
    struct { const node_t* nullable n[2]; } tuple;
    struct { const node_t* n; const nodearray_t* na; } one_array;
    struct { const node_t*const* v; u32 len, index; } array;
    struct {
      const void* p;
      u8          step;
      u8          data_u8[3];
      u32         data_u32;
    } custom;
  };
} astiterx_t;


static_assert(sizeof(void*) <= sizeof(u64), "");
static_assert(sizeof(astiter_t) == sizeof(astiterx_t), "");


// Currently no iterators need cleanup, but if they ever did,
// astiter_dispose could be implemented like this to guarantee that
// an iterator reaches its final step (which would do the cleanup.)
//
//   void astiter_dispose(astiter_t* itp) {
//     astiterx_t* it = (astiterx_t*)itp;
//     // unwind
//     while (it->f && it->f(it)) {}
//   }


static const node_t* it_end(astiterx_t* it) {
  return NULL;
}


static const node_t* it_1(astiterx_t* it) {
  it->f = it_end;
  return it->tuple.n[0];
}

static void mkit_1(astiterx_t* it, const void* np) {
  it->f = it_1;
  it->tuple.n[0] = np;
}


static const node_t* it_2(astiterx_t* it) {
  const node_t* n = it->tuple.n[0];
  if (it->tuple.n[1]) {
    it->tuple.n[0] = it->tuple.n[1];
    it->tuple.n[1] = NULL;
  } else {
    it->f = it_end;
  }
  return n;
}

static void mkit_2(astiterx_t* it, const void* n0, const void* nullable n1) {
  it->f = it_2;
  it->tuple.n[0] = n0;
  it->tuple.n[1] = n1;
}


static const node_t* it_array(astiterx_t* it) {
  if (it->array.index == it->array.len)
    return NULL;
  return it->array.v[it->array.index++];
}

static void mkit_array(astiterx_t* it, const nodearray_t* na) {
  it->f = it_array;
  it->array.v = (const node_t*const*)na->v;
  it->array.len = na->len;
  it->array.index = 0;
}


static const node_t* it_1_array(astiterx_t* it) {
  const node_t* n = it->one_array.n;
  mkit_array(it, it->one_array.na);
  return n;
}

static void mkit_1_array(
  astiterx_t* it, const void* nullable n, const nodearray_t* na)
{
  if (n) {
    it->f = it_1_array;
    it->one_array.n = n;
    it->one_array.na = na;
  } else {
    mkit_array(it, na);
  }
}


#define CUSTOM_STEP_0(T, NAME) \
  { const T* NAME = it->custom.p; \
    switch (it->custom.step) { \
    case 0: it->custom.step++; /* logic ... */

#define CUSTOM_STEP(STEP) \
  FALLTHROUGH; case STEP: it->custom.step++; /* logic ... */

#define CUSTOM_END() \
  break; } return NULL; }


static const node_t* it_ifexpr(astiterx_t* it) {
  CUSTOM_STEP_0(ifexpr_t, n) return (const node_t*)n->cond;
  CUSTOM_STEP(1)             return (const node_t*)n->thenb;
  CUSTOM_STEP(2)             if (n->elseb) return (const node_t*)n->elseb;
  CUSTOM_END()
}

static const node_t* it_forexpr(astiterx_t* it) {
  CUSTOM_STEP_0(forexpr_t, n) if (n->start) return (const node_t*)n->start;
  CUSTOM_STEP(1)              return (const node_t*)n->cond;
  CUSTOM_STEP(2)              return (const node_t*)n->body;
  CUSTOM_STEP(3)              if (n->end) return (const node_t*)n->end;
  CUSTOM_END()
}

static const node_t* it_fun(astiterx_t* it) {
  CUSTOM_STEP_0(fun_t, n) if (n->recvt) return (const node_t*)n->recvt;
  CUSTOM_STEP(1)          if (n->body) return (const node_t*)n->body;
  // CUSTOM_STEP(2)          if (n->nsparent) return n->nsparent;
  CUSTOM_END()
}


astiter_t astiter_of_children(const node_t* n) {
  astiterx_t it;
  const void* np = n;
  // dlog("%s %s %p", __FUNCTION__, nodekind_name(n->kind), n);

  #define MKIT_1(T, MEMBER)       mkit_1(&it,((T*)n)->MEMBER); break
  #define MKIT_2(T, M1, M2)       mkit_2(&it,((T*)n)->M1, ((T*)n)->M2); break
  #define MKIT_ARRAY(T, MEMBER)   mkit_array(&it, &((T*)n)->MEMBER); break
  #define MKIT_1_ARRAY(T, NM, AM) mkit_1_array(&it, ((T*)n)->NM, &((T*)n)->AM); break

  switch ((enum nodekind)n->kind) {

  case NODE_UNIT:     MKIT_ARRAY(unit_t, children);
  case STMT_TYPEDEF:  MKIT_1(typedef_t, type);
  case EXPR_ARRAYLIT: MKIT_ARRAY(arraylit_t, values);
  case EXPR_BLOCK:    MKIT_ARRAY(block_t, children);

  case EXPR_ASSIGN:
  case EXPR_BINOP: MKIT_2(binop_t, left, right);

  case EXPR_DEREF:
  case EXPR_POSTFIXOP:
  case EXPR_PREFIXOP: MKIT_1(unaryop_t, expr);

  case EXPR_ID:     MKIT_1(idexpr_t, ref);
  case EXPR_NS:     MKIT_ARRAY(nsexpr_t, members);
  case EXPR_RETURN: MKIT_1(retexpr_t, value);

  case EXPR_VAR:
  case EXPR_LET:
  case EXPR_PARAM:
  case EXPR_FIELD: MKIT_1(local_t, init);

  case EXPR_CALL: MKIT_1_ARRAY(call_t, recv, args);

  case EXPR_IF:
    it.f = it_ifexpr;
    it.custom.p = n;
    it.custom.step = 0;
    break;

  case EXPR_FOR:
    it.f = it_forexpr;
    it.custom.p = n;
    it.custom.step = 0;
    break;

  case EXPR_FUN:
    it.f = it_fun;
    it.custom.p = n;
    it.custom.step = 0;
    break;

  case EXPR_MEMBER:    MKIT_2(member_t, recv, target);
  case EXPR_SUBSCRIPT: MKIT_2(subscript_t, recv, index);

  case EXPR_TYPECONS: { const typecons_t* n = np;
    // when type is primitive, argument is stored at n->expr, else in array at args
    if (!n->type) {
      it.f = it_forexpr;
    } else if (type_isprim(n->type)) {
      mkit_1(&it, n->expr);
    } else {
      mkit_array(&it, &n->args);
    }
    break;
  }

  // case TYPE_ALIAS: MKIT_2(aliastype_t, elem, nsparent);
  case TYPE_ALIAS: MKIT_1(aliastype_t, elem);

  case TYPE_ARRAY: MKIT_1(arraytype_t, lenexpr);
  case TYPE_FUN:   MKIT_1_ARRAY(funtype_t, result, params);

  case TYPE_PTR:
  case TYPE_REF:
  case TYPE_MUTREF:
  case TYPE_OPTIONAL:
  case TYPE_SLICE:
  case TYPE_MUTSLICE: MKIT_1(ptrtype_t, elem);

  // case TYPE_STRUCT:     MKIT_1_ARRAY(structtype_t, nsparent, fields);
  case TYPE_STRUCT:     MKIT_ARRAY(structtype_t, fields);
  case TYPE_NS:         MKIT_ARRAY(nstype_t, members);

  case TYPE_UNRESOLVED: MKIT_1(unresolvedtype_t, resolved);

  // no children
  case NODE_BAD:
  case NODE_COMMENT:
  case NODE_IMPORTID:
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
    it.f = it_end;
    break;
  }

  return *(astiter_t*)&it;
}
