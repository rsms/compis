// SPDX-License-Identifier: Apache-2.0
#include "c0lib.h"
#include "compiler.h"


bool types_isconvertible(const type_t* dst, const type_t* src) {
  assertnotnull(dst);
  assertnotnull(src);
  if (dst == src)
    return true;
  if (type_isprim(dst) && type_isprim(src))
    return true;
  return false;
}


bool _types_iscompat(const type_t* dst, const type_t* src) {
  assertnotnull(dst);
  assertnotnull(src);

  while (dst->kind == TYPE_ALIAS)
    dst = assertnotnull(((aliastype_t*)dst)->elem);

  while (src->kind == TYPE_ALIAS)
    src = assertnotnull(((aliastype_t*)src)->elem);

  switch (dst->kind) {
    case TYPE_INT:
    case TYPE_I8:
    case TYPE_I16:
    case TYPE_I32:
    case TYPE_I64:
      return (dst == src) && (dst->isunsigned == src->isunsigned);
    case TYPE_PTR:
      // *T <= *T
      // &T <= *T
      return (
        (src->kind == TYPE_PTR || src->kind == TYPE_REF) &&
        types_iscompat(((const ptrtype_t*)dst)->elem, ((const ptrtype_t*)src)->elem) );
    case TYPE_REF: {
      // &T    <= &T
      // mut&T <= &T
      // mut&T <= mut&T
      // &T    x= mut&T
      // &T    <= *T
      // mut&T <= *T
      const reftype_t* d = (const reftype_t*)dst;
      if (src->kind == TYPE_PTR)
        return types_iscompat(d->elem, ((const ptrtype_t*)src)->elem);
      const reftype_t* s = (const reftype_t*)src;
      return (
        src->kind == TYPE_REF &&
        (s->ismut == d->ismut || s->ismut || !d->ismut) &&
        types_iscompat(d->elem, s->elem) );
    }
    case TYPE_OPTIONAL: {
      // ?T <= T
      // ?T <= ?T
      const opttype_t* d = (const opttype_t*)dst;
      if (src->kind == TYPE_OPTIONAL)
        src = ((const opttype_t*)src)->elem;
      return types_iscompat(d->elem, src);
    }
  }
  return dst == src;
}


bool typeid_append(buf_t* buf, type_t* t) {
  if (type_isprim(t))
    return buf_push(buf, (u8)t->tid[0]);
  return buf_print(buf, _typeid(t));
}


sym_t _typeid(type_t* t) {
  if (t->tid)
    return t->tid;

  char storage[64];
  buf_t buf = buf_makeext(memalloc_ctx(), storage, sizeof(storage));

  buf_push(&buf, TYPEID_PREFIX(t->kind));

  switch (t->kind) {
    case TYPE_FUN:
      assertf(0,"funtype should have precomputed typeid");
      break;
    case TYPE_PTR:
      if (!typeid_append(&buf, ((ptrtype_t*)t)->elem))
        goto oom;
      break;
    case TYPE_REF:
      if (!typeid_append(&buf, ((reftype_t*)t)->elem))
        goto oom;
      break;
    case TYPE_OPTIONAL:
      if (!typeid_append(&buf, ((opttype_t*)t)->elem))
        goto oom;
      break;
    case TYPE_STRUCT: {
      structtype_t* st = (structtype_t*)t;
      if UNLIKELY(!buf_print_leb128_u32(&buf, st->fields.len))
        goto oom;
      for (u32 i = 0; i < st->fields.len; i++) {
        local_t* field = st->fields.v[i];
        assert(field->kind == EXPR_FIELD);
        if UNLIKELY(!typeid_append(&buf, assertnotnull(field->type)))
          goto oom;
      }
      break;
    }
    default:
      dlog("TODO create typeid for %s", nodekind_name(t->kind));
      buf_dispose(&buf);
      t->tid = sym__;
      return sym__;
  }

  #if 0 && DEBUG
    char tmp[128];
    abuf_t s = abuf_make(tmp, sizeof(tmp));
    abuf_repr(&s, buf.p, buf.len);
    abuf_terminate(&s);
    dlog("build typeid %s \"%s\"", nodekind_name(t->kind), tmp);
  #endif

  sym_t tid = sym_intern(buf.p, buf.len);
  buf_dispose(&buf);
  t->tid = tid;
  return tid;
oom:
  fprintf(stderr, "out of memory\n");
  buf_dispose(&buf);
  return NULL;
}
