#include "c0lib.h"
#include "compiler.h"


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
