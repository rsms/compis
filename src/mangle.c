// SPDX-License-Identifier: Apache-2.0
#include "colib.h"
#include "compiler.h"
#include "hashtable.h"
#include "hash.h"

// see https://refspecs.linuxbase.org/cxxabi-1.86.html#mangling
// see https://rust-lang.github.io/rfcs/2603-rust-symbol-name-mangling-v0.html


typedef struct {
  const node_t*  st[8];
  const node_t** v;
  u32            len, cap;
} nsstack_t;

typedef struct {
  const node_t* n;
  usize         offs; // byte offset into buf
} offstab_ent_t;

typedef struct {
  const compiler_t* c;
  const pkg_t*      pkg;
  buf_t             buf;
  nsstack_t         nsstack;
  hashtable_t       offstab; // table of offstab_ent_t
} encoder_t;


#define TEMPLATE_TAG  'T' // template type, e.g. "type Foo<T> {}"
#define TEMPLATEI_TAG 'I' // instance of template, e.g. "var x Foo<int>"
#define BACKREF_TAG   'B' // back reference "B<base-62-number>_"

// tags
//
// ——IMPORTANT——
// Changing these will:
// - REQUIRE manual update of type definitions in coprelude.h
// - invalidate all existing metafiles
//
static u8 tagtab[NODEKIND_COUNT] = {
  [NODE_UNIT] = 'M',
  // primitive types use lower-case characters
  [TYPE_VOID] = 'z',
  [TYPE_BOOL] = 'b',
  [TYPE_I8]   = 'a',  [TYPE_U8]   = 'h',
  [TYPE_I16]  = 's',  [TYPE_U16]  = 't',
  [TYPE_I32]  = 'l',  [TYPE_U32]  = 'm',
  [TYPE_I64]  = 'x',  [TYPE_U64]  = 'y',
  [TYPE_INT]  = 'i',  [TYPE_UINT] = 'j',
  [TYPE_F32]  = 'f',
  [TYPE_F64]  = 'd',

  // all other types use upper-case characters (must be <='Z')
  [TYPE_PTR]         = 'P',
  [TYPE_REF]         = 'R',
  [TYPE_OPTIONAL]    = 'O',
  [TYPE_ARRAY]       = 'A',
  [TYPE_SLICE]       = 'S',
  [TYPE_MUTSLICE]    = 'D',
  [TYPE_ALIAS]       = 'A',
  [TYPE_FUN]         = 'F',
  [TYPE_STRUCT]      = 'N',
  [EXPR_FUN]         = 'N',
  [TYPE_PLACEHOLDER] = 'H',
  [TYPE_TEMPLATE]    = TEMPLATEI_TAG,
};


static bool offstab_ent_eqfn(const void* ent1, const void* ent2) {
  return ((offstab_ent_t*)ent1)->n == ((offstab_ent_t*)ent2)->n;
}

static usize offstab_ent_hashfn(usize seed, const void* entp) {
  const offstab_ent_t* ent = entp;
  return wyhash64((uintptr)ent->n, seed);
}


static void type(encoder_t* e, const type_t* t);


static void append_zname(encoder_t* e, const char* name) {
  usize len = strlen(name);
  assert(len <= U32_MAX);
  buf_print_u32(&e->buf, (u32)len, 10);
  buf_append(&e->buf, name, len);
}


static usize offstab_add(encoder_t* e, const node_t* n, usize offs) {
  offstab_ent_t offstab_key = { .n=n, .offs=offs };
  bool added;
  offstab_ent_t* ent = hashtable_assign(
    &e->offstab, offstab_ent_hashfn, offstab_ent_eqfn,
    sizeof(offstab_ent_t), &offstab_key, &added);
  if (!ent) {
    e->buf.oom = true;
    return 0;
  }
  if (!added)
    return ent->offs;
  return 0;
}


static void start_path(encoder_t* e, const node_t* n) {
  UNUSED usize offs = offstab_add(e, n, e->buf.len);
  assertf(offs == 0, "duplicate");
  if (n->flags & NF_TEMPLATEI)
    buf_push(&e->buf, TEMPLATEI_TAG);
  if (n->flags & NF_TEMPLATE)
    buf_push(&e->buf, TEMPLATE_TAG);
  u8 tag = tagtab[n->kind];
  assertf(tag, "missing tag for %s", nodekind_name(n->kind));
  buf_push(&e->buf, tag);
  if (tag < 'a') switch (n->kind) {
    case TYPE_STRUCT: buf_push(&e->buf, 's'); break;
    case EXPR_FUN:    buf_push(&e->buf, 'f'); break;
  }
}


// mangle_str writes name to buf, escaping any chars not in 0-9A-Za-z_
// by "$XX" where XX is the hexadecimal encoding of the byte value,
// or "·" (U+00B7, UTF-8 C2 B7) for '/' and '\'
bool mangle_str(buf_t* buf, slice_t name) {
  usize i = 0, dstlen;
  bool ok = true;

  for (; i < name.len; i++) {
    u8 c = name.bytes[i];
    if (!isalnum(c) && c != '_')
      goto escape;
  }

  // only 0-9A-Za-z_
  ok &= buf_print_u32(buf, name.len, 10);
  ok &= buf_append(buf, name.p, name.len);
  return ok;

escape:
  dstlen = i;
  for (usize j = i; j < name.len; j++) {
    u8 c = name.bytes[j];
    usize isslash = (1llu * (usize)(c == '/' || c == '\\'));
    usize isspecial = (2llu * (usize)(!isalnum(c) && c != '_'));
    dstlen += 1llu + isspecial*(usize)!isslash + isslash;
  }

  ok &= buf_print_u32(buf, dstlen, 10);
  ok &= buf_reserve(buf, dstlen);
  if (!ok)
    return ok;

  char* dst = buf->chars + buf->len;
  memcpy(dst, name.p, i);
  dst += i;

  static const char* kHexchars = "0123456789ABCDEF";

  for (; i < name.len; i++) {
    u8 c = name.bytes[i];
    if (isalnum(c) || c == '_') {
      *dst++ = c;
    } else if (c == '/' || c == '\\') {
      dst[0] = 0xC2; dst[1] = 0xB7; // UTF-8 of U+00B7 MIDDLE DOT "·"
      dst += 2;
    } else {
      dst[0] = '$';
      dst[1] = kHexchars[c >> 4];
      dst[2] = kHexchars[c & 0xf];
      dst += 3;
    }
  }

  usize wlen = (usize)(uintptr)(dst - (buf->chars + buf->len));
  buf->len += wlen;

  return ok;
}


static void append_pkgname(encoder_t* e) {
  mangle_str(&e->buf, str_slice(e->pkg->path));
}


static void end_path(encoder_t* e, const node_t* n) {
  switch (n->kind) {
  case NODE_UNIT:
    append_pkgname(e);
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
  case TYPE_ALIAS:
    append_zname(e, ((aliastype_t*)n)->name);
    break;
  }

  // append template instance arguments
  if (n->flags & NF_TEMPLATEI) {
    assert(nodekind_isusertype(n->kind));
    const usertype_t* t = (usertype_t*)n;
    for (u32 i = 0; i < t->templateparams.len; i++) {
      // TODO: support expressions, e.g. "type Foo<Size> {...}; var x Foo<123>"
      assert(node_istype(t->templateparams.v[i]));
      type(e, (type_t*)t->templateparams.v[i]);
    }
  }
}


static void funtype1(encoder_t* e, const funtype_t* ft) {
  // e.g. "fun (x, y i32) i8" => "TiiEa"
  if (ft->params.len) {
    buf_push(&e->buf, 'T');
    for (u32 i = 0; i < ft->params.len; i++) {
      const local_t* param = (local_t*)ft->params.v[i];
      type(e, param->type);
    }
    buf_push(&e->buf, 'E');
  }
  type(e, ft->result);
}


static const char k_base62chars[] = {
  "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz"};

// 0xffffffffffffffff => "LygHa16AHYF"
#define BASE62_U64_MAX 11

static usize base62_enc_u64(char dst[BASE62_U64_MAX], u64 val) {
  char* p = dst;
  u64 rem;
  while (val > 0) {
    rem = val % 62;
    val = val / 62;
    *p++ = k_base62chars[rem];
  }
  return (usize)(uintptr)(p - dst);
}


static void type(encoder_t* e, const type_t* t) {
  assert(t->kind < NODEKIND_COUNT);
  assert(nodekind_istype(t->kind));
  //dlog("mangle type %s#%p", nodekind_name(t->kind), t);

  u8 tag = tagtab[t->kind];
  assertf(tag, "missing tag for %s", nodekind_name(t->kind));

  // primitive types use lower-case characters
  if (tag > 'Z') {
    buf_push(&e->buf, tag);
    return;
  }


  // TODO: compression; when the same name appears an Nth time, refer to the first
  // definition instead of printing it again.
  // e.g. instead of encoding "Foo<Bar,Bar>" as
  //   INsM7example3FooYNsM7example3BarYNsM7example3Bar
  // we should use back references for repeated names:
  //
  //      ——span1——    ——span2———
  //   INsM7example3FooYNsB3_3BarBG_
  //                      ~~~    ~~~
  //                       1      2
  //
  // In Rust, back references have the form "B<base-62-number>_"
  // See: C++/itanium and Rust does something like this.
  // https://rust-lang.github.io/rfcs/
  //   2603-rust-symbol-name-mangling-v0.html#compressionsubstitution
  //
  UNUSED usize offs = offstab_add(e, (node_t*)t, e->buf.len);
  if (offs > 0) {
    if (buf_reserve(&e->buf, BASE62_U64_MAX+2)) {
      e->buf.chars[e->buf.len++] = BACKREF_TAG;
      e->buf.len += base62_enc_u64(&e->buf.chars[e->buf.len], offs);
      e->buf.chars[e->buf.len++] = '_';
    }
    //dlog("backref '%.*s'", (int)e->buf.len, e->buf.chars);
    return;
  }

  switch (t->kind) {

  case TYPE_PTR:
  case TYPE_REF:
  case TYPE_MUTREF:
  case TYPE_SLICE:
  case TYPE_MUTSLICE:
  case TYPE_ARRAY:
  case TYPE_OPTIONAL:
    buf_push(&e->buf, tag);
    type(e, ((ptrtype_t*)t)->elem);
    break;

  case TYPE_STRUCT: {
    const structtype_t* st = (structtype_t*)t;
    if (st->mangledname) {
      buf_print(&e->buf, st->mangledname);
    } else {
      // anonymous struct
      //   TAG nfields typeof(field0) typeof(field1) ... typeof(fieldN)
      buf_push(&e->buf, tag);
      buf_print_u32(&e->buf, st->fields.len, 10);
      for (u32 i = 0; i < st->fields.len; i++) {
        const local_t* field = (local_t*)st->fields.v[i];
        type(e, field->type);
      }
    }
    break;
  }

  case TYPE_FUN:
    buf_push(&e->buf, tag);
    funtype1(e, (funtype_t*)t);
    break;

  case TYPE_TEMPLATE: {
    // templatetype_t is an instantation of a template
    const templatetype_t* tt = (templatetype_t*)t;
    buf_push(&e->buf, tag);
    type(e, (type_t*)tt->recv);
    for (u32 i = 0; i < tt->args.len; i++)
      type(e, (type_t*)tt->args.v[i]);
    break;
  }

  case TYPE_PLACEHOLDER:
    buf_push(&e->buf, tag);
    append_zname(e, ((placeholdertype_t*)t)->templateparam->name);
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
  if (!newv)
    return false;
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


static bool encoder_init(
  encoder_t* e, const compiler_t* c, const pkg_t* pkg, buf_t* buf)
{
  memset(e, 0, sizeof(*e));

  e->c = c;
  e->pkg = pkg;
  e->buf = *buf;
  e->nsstack.v = e->nsstack.st;
  e->nsstack.cap = countof(e->nsstack.st);

  err_t err = hashtable_init(&e->offstab, e->c->ma, sizeof(offstab_ent_t), 16);

  return err == 0;
}


static bool encoder_finalize(encoder_t* e, buf_t* buf) {
  if (e->nsstack.v != e->nsstack.st) {
    mem_freev(e->c->ma, e->nsstack.v, e->nsstack.cap, sizeof(void*));
    e->nsstack.v = 0;
    e->nsstack.cap = 0;
  }

  hashtable_dispose(&e->offstab, sizeof(offstab_ent_t));
  buf_nullterm(&e->buf);

  bool ok = !e->buf.oom;
  e->buf.oom = false;
  *buf = e->buf;

  return ok;
}


bool compiler_mangle_type(
  const compiler_t* c, const pkg_t* pkg, buf_t* buf, const type_t* t)
{
  encoder_t e;
  if (!encoder_init(&e, c, pkg, buf))
    return false;
  buf_reserve(buf, 16);
  type(&e, t);
  return encoder_finalize(&e, buf);
}


static bool encoder_finalize_ptrtype(encoder_t* e, buf_t* buf, const ptrtype_t* t) {
  type(e, t->elem);
  //buf_print(&e->buf, CO_TYPE_SUFFIX);
  return encoder_finalize(e, buf);
}


bool compiler_mangle(
  const compiler_t* c, const pkg_t* pkg, buf_t* buf, const node_t* n)
{
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

  if (n->kind == EXPR_FUN && ((fun_t*)n)->abi == ABI_C)
    return buf_print(buf, ((fun_t*)n)->name);

  encoder_t e;
  if (!encoder_init(&e, c, pkg, buf))
    return false;

  buf_reserve(&e.buf, 64);

  // // common prefix
  // buf_push(&e.buf, '_');
  // buf_push(&e.buf, 'C');

  switch (n->kind) {
    case TYPE_OPTIONAL:
      buf_print(&e.buf, CO_TYPE_PREFIX "opt_");
      return encoder_finalize_ptrtype(&e, buf, (ptrtype_t*)n);
    case TYPE_ARRAY:
      buf_print(&e.buf, CO_TYPE_PREFIX "array_");
      return encoder_finalize_ptrtype(&e, buf, (ptrtype_t*)n);
    case TYPE_SLICE:
      buf_print(&e.buf, CO_TYPE_PREFIX);
      buf_print(&e.buf, // "mutslice_" or "slice_"
        &"mutslice_"[3lu*(usize)(((slicetype_t*)n)->kind == TYPE_SLICE)]);
      return encoder_finalize_ptrtype(&e, buf, (ptrtype_t*)n);
  }

  // path
  const node_t* ns = n;
  for (;;) {
    start_path(&e, ns);
    nsstack_push(&e, ns);
    switch (ns->kind) {
      case EXPR_FUN:    ns = assertnotnull(((fun_t*)ns)->nsparent); break;
      case TYPE_STRUCT: ns = assertnotnull(((structtype_t*)ns)->nsparent); break;
      case TYPE_ALIAS:  ns = assertnotnull(((aliastype_t*)ns)->nsparent); break;

      case NODE_UNIT:
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
        goto endpath;
      default:
        assertf(0, "unexpected %s", nodekind_name(ns->kind));
    }
  }
endpath:
  for (u32 i = e.nsstack.len; i;)
    end_path(&e, e.nsstack.v[--i]);

  return encoder_finalize(&e, buf);
}
