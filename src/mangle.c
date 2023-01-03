// SPDX-License-Identifier: Apache-2.0
#include "colib.h"
#include "compiler.h"

// see https://refspecs.linuxbase.org/cxxabi-1.86.html#mangling
// see https://rust-lang.github.io/rfcs/2603-rust-symbol-name-mangling-v0.html


typedef struct {
  const node_t*  st[8];
  const node_t** v;
  u32            len, cap;
} nsstack_t;

typedef struct {
  const compiler_t* c;
  buf_t             buf;
  nsstack_t         nsstack;
  bool              ok;
} encoder_t;


static u8 tagtab[NODEKIND_COUNT] = {
  [NODE_UNIT] = 'M',
  [TYPE_VOID] = 'z',
  [TYPE_BOOL] = 'b',
  [TYPE_I8]   = 'a',  [TYPE_U8]   = 'h',
  [TYPE_I16]  = 's',  [TYPE_U16]  = 't',
  [TYPE_I32]  = 'l',  [TYPE_U32]  = 'm',
  [TYPE_I64]  = 'x',  [TYPE_U64]  = 'y',
  [TYPE_INT]  = 'i',  [TYPE_UINT] = 'j',
  [TYPE_F32]  = 'f',
  [TYPE_F64]  = 'd',
  // [TYPE_ARRAY] = '?',
  [TYPE_PTR] = 'P',
  [TYPE_REF] = 'R',
  // [TYPE_OPTIONAL] = '?',
  // [TYPE_ALIAS]    = '?',
  [TYPE_FUN] = 'F',
  [TYPE_STRUCT] = 'N',
  [EXPR_FUN]    = 'N',
};


static void type(encoder_t* e, const type_t* t);


static void append_zname(encoder_t* e, const char* name) {
  usize len = strlen(name);
  assert(len <= U32_MAX);
  e->ok &= buf_print_u32(&e->buf, (u32)len, 10);
  e->ok &= buf_append(&e->buf, name, len);
}


static void start_path(encoder_t* e, const node_t* n) {
  u8 tag = tagtab[n->kind];
  assertf(tag, "missing tag for %s", nodekind_name(n->kind));
  buf_push(&e->buf, tag);
  if (tag < 'a') switch (n->kind) {
    case TYPE_STRUCT: buf_push(&e->buf, 's'); break;
    case EXPR_FUN:    buf_push(&e->buf, 'f'); break;
  }
}


static void end_path(encoder_t* e, const node_t* n) {
  switch (n->kind) {
  case NODE_UNIT:
    append_zname(e, e->c->pkgname);
    break;
  case EXPR_FUN:
    assertf(((fun_t*)n)->name, "unnamed %s in ns path", nodekind_name(n->kind));
    append_zname(e, ((fun_t*)n)->name);
    break;
  case TYPE_STRUCT:
    if (((structtype_t*)n)->name) {
      append_zname(e, ((structtype_t*)n)->name);
    } else {
      dlog("TODO unnamed type");
    }
    break;
  }
}


static void funtype1(encoder_t* e, const funtype_t* ft) {
  // e.g. "fun (x, y i32) i8" => "TiiEa"
  if (ft->params.len) {
    buf_push(&e->buf, 'T');
    for (u32 i = 0; i < ft->params.len; i++) {
      const local_t* param = ft->params.v[i];
      type(e, param->type);
    }
    buf_push(&e->buf, 'E');
  }
  type(e, ft->result);
}


static void type(encoder_t* e, const type_t* t) {
  assert(t->kind < NODEKIND_COUNT);
  u8 tag = tagtab[t->kind];
  assertf(tag, "missing tag for %s", nodekind_name(t->kind));
  if (tag > 'Z') {
    buf_push(&e->buf, tag);
    return;
  }
  switch (t->kind) {
    case TYPE_REF:
      buf_push(&e->buf, tag);
      type(e, ((reftype_t*)t)->elem);
      break;
    case TYPE_STRUCT: {
      const structtype_t* st = (structtype_t*)t;
      if (st->mangledname) {
        e->ok &= buf_print(&e->buf, st->mangledname);
      } else {
        assertf(0, "TODO unnamed type");
      }
      break;
    }
    case TYPE_FUN:
      buf_push(&e->buf, tag);
      funtype1(e, (funtype_t*)t);
      break;
    default:
      assertf(0, "unexpected %s tag='%C'", nodekind_name(t->kind), tag);
  }
}


static bool nsstack_grow(encoder_t* e) {
  usize cap = (usize)e->nsstack.cap;
  void* oldv = e->nsstack.v;
  void* newv;
  if (oldv == e->nsstack.st) {
    newv = mem_allocv(e->c->ma, cap*2, sizeof(void*));
    memcpy(newv, oldv, sizeof(void*)*cap);
  } else {
    newv = mem_resizev(e->c->ma, oldv, cap, cap*2, sizeof(void*));
  }
  if (!newv) {
    e->ok = false;
    return false;
  }
  e->nsstack.v = newv;
  e->nsstack.cap = cap*2;
  return true;
}


static bool nsstack_push(encoder_t* e, const node_t* n) {
  if UNLIKELY(e->nsstack.cap == e->nsstack.len && !nsstack_grow(e))
    return false;
  e->nsstack.v[e->nsstack.len++] = n;
  return true;
}


static encoder_t mkencoder(const compiler_t* c, buf_t* buf) {
  encoder_t e = {
    .c = c,
    .buf = *buf,
  };
  e.nsstack.v = e.nsstack.st;
  e.nsstack.cap = countof(e.nsstack.st);
  return e;
}


static bool encoder_finalize(encoder_t* e, buf_t* buf) {
  if (e->nsstack.v != e->nsstack.st) {
    mem_freev(e->c->ma, e->nsstack.v, e->nsstack.cap, sizeof(void*));
    e->nsstack.v = 0;
    e->nsstack.cap = 0;
  }
  bool ok = buf_nullterm(&e->buf);
  *buf = e->buf;
  return ok & e->ok;
}


bool compiler_mangle_type(const compiler_t* c, buf_t* buf, const type_t* t) {
  encoder_t e = mkencoder(c, buf);
  buf_reserve(buf, 16);
  type(&e, t);
  return encoder_finalize(&e, buf);
}


bool compiler_mangle(const compiler_t* c, buf_t* buf, const node_t* n) {
  // package mypkg
  // namespace foo {
  //   fun bar() {}
  // }
  //
  // _CNfNnM5mypkg3foo3barz
  // ├┘│││││└─┬──┘└─┬┘└─┬┘│
  // │ │││││  │     │   │ └─ "void" result type
  // │ │││││  │     │   └─── "bar" identifier
  // │ │││││  │     └─────── "foo" identifier
  // │ │││││  └───────────── "mypkg" identifier
  // │ ││││└──────────────── start tag for "mypkg"
  // │ │││└───────────────── namespace tag for "foo"
  // │ ││└────────────────── start tag for "foo"
  // │ │└─────────────────── namespace tag for "bar"
  // │ └──────────────────── start tag for "bar"
  // └────────────────────── common symbol prefix
  //

  encoder_t e = mkencoder(c, buf);

  buf_reserve(buf, 64);

  // // common prefix
  // buf_push(buf, '_');
  // buf_push(buf, 'C');

  // path
  const node_t* ns = n;
  for (;;) {
    start_path(&e, ns);
    nsstack_push(&e, ns);
    switch (ns->kind) {
      case EXPR_FUN:    ns = assertnotnull(((fun_t*)ns)->nsparent); break;
      case TYPE_STRUCT: ns = assertnotnull(((structtype_t*)ns)->nsparent); break;
      case NODE_UNIT:   goto endpath;
      default:          assertf(0, "unexpected %s", nodekind_name(ns->kind));
    }
  }
endpath:
  for (u32 i = e.nsstack.len; i;)
    end_path(&e, e.nsstack.v[--i]);

  // TODO: append types for generic functions, structs, arrays:
  // // type
  // if (n->kind == EXPR_FUN) {
  //   // e.g. "fun foo(x, y i32) i8" => "Nf3fooTiiEa"
  //   funtype1(&e, (funtype_t*)((fun_t*)n)->type);
  // }

  return encoder_finalize(&e, buf);
}
