// SPDX-License-Identifier: Apache-2.0
#include "colib.h"
#include "compiler.h"


static bool type_isowner_safe1(const type_t* t, u32 n) {
  t = type_isopt(t) ? ((opttype_t*)t)->elem : t;
  return (
    (t->flags & (NF_DROP | NF_SUBOWNERS)) ||
    type_isptr(t) ||
    ( t->kind == TYPE_ALIAS &&
      n > 0 && type_isowner_safe1(((aliastype_t*)t)->elem, n - 1) )
  );
}

static bool type_isowner_safe(const type_t* t) {
  return type_isowner_safe1(t, 16);
}


static void error_ownership_cycle_help(
  compiler_t* c, const type_t* bt, const node_t* nullable origin_n)
{
  origin_t origin;
  #define HELP(fmt, args...) report_diag(c, origin, DIAG_HELP, fmt, ##args)
  if (origin_n == NULL) {
    origin = ast_origin(&c->locmap, (node_t*)bt);
    HELP("type %s defined here", fmtnode(1, bt));
    return;
  }

  origin = ast_origin(&c->locmap, origin_n);

  const char* bt_kind_prefix = type_isowner_safe(bt) ? "managed-lifetime " : "";

  switch (origin_n->kind) {
  case EXPR_FIELD:
    HELP("field \"%s\" of %s%s %s",
      ((local_t*)origin_n)->name,
      nodekind_fmt(bt->kind), bt_kind_prefix, fmtnode(0, bt));
    break;
  case TYPE_ALIAS:
    HELP("type alias \"%s\" of %s%s %s",
      ((aliastype_t*)origin_n)->name,
      nodekind_fmt(bt->kind), bt_kind_prefix, fmtnode(0, bt));
    break;
  case TYPE_ARRAY:
    HELP("array of %s%s %s",
      nodekind_fmt(bt->kind), bt_kind_prefix, fmtnode(0, bt));
    break;
  default:
    HELP("%s %s of %s%s %s",
      nodekind_fmt(origin_n->kind), fmtnode(1, origin_n),
      nodekind_fmt(bt->kind), bt_kind_prefix, fmtnode(0, bt));
  }
  #undef HELP
}


static bool error_ownership_cycle(
  compiler_t*          c,
  nodearray_t*         defs,
  u32                  vstk_base,
  const type_t*        bt,
  const void* nullable origin_n)
{
  // find previous occurrance of bt in visit stack
  u32 index = vstk_base;
  for (; index < defs->len; index++) {
    if (defs->v[index] == (node_t*)bt)
      break;
  }
  assertf(index < defs->len, "not found");

  // generate helpful "path"
  buf_t buf = buf_make(c->ma);
  buf_print(&buf, " (");
  for (u32 i = index; i < defs->len; i++) {
    node_fmt(&buf, (node_t*)defs->v[i], /*depth*/0);
    buf_print(&buf, " -> ");
  }
  node_fmt(&buf, (node_t*)bt, /*depth*/0);
  buf_print(&buf, ")");
  if (buf.oom)
    buf.len = 0;

  if (!origin_n)
    origin_n = bt;

  // emit diagnostic (error)
  if (type_isowner_safe(bt)) {
    report_diag(c, ast_origin(&c->locmap, (node_t*)origin_n), DIAG_ERR,
      "ownership cycle: %s manages its own lifetime%.*s",
      fmtnode(0, bt), (int)buf.len, buf.chars);
  } else {
    report_diag(c, ast_origin(&c->locmap, (node_t*)origin_n), DIAG_ERR,
      "interdependent type %s%.*s",
      fmtnode(0, bt), (int)buf.len, buf.chars);
  }

  buf_dispose(&buf);

  return false;
}


static bool check_type(
  compiler_t*          c,
  nodearray_t*         defs,
  u32                  vstk_base,
  u32                  aliasnest,
  const type_t*        t,
  const void* nullable origin)
{
  // bt becomes the "bottom type" of t, e.g. ?*T => T
  const type_t* bt = t;
unwrap:
  switch (bt->kind) {
    case TYPE_OPTIONAL:
    case TYPE_PTR:
      bt = assertnotnull(((ptrtype_t*)bt)->elem);
      goto unwrap;

    // we will inspect these types closer as they may contain subtypes
    case TYPE_ARRAY:
    case TYPE_STRUCT:
    case TYPE_ALIAS:
    case TYPE_TEMPLATE:
      break;

    // check reference types when "inside" an alias
    case TYPE_REF:
    case TYPE_MUTREF:
    case TYPE_SLICE:
    case TYPE_MUTSLICE:
      if (aliasnest > 0)
        break;
      FALLTHROUGH;

    // other types cannot cause cycles
    default:
      //dlog("[%s] skip %s", __FUNCTION__, fmtnode(0, t));
      return true;
  }

  //dlog("[%s] %s (%s)", __FUNCTION__, fmtnode(0, t), fmtnode(1, bt));
  u32 i;

  // is bt on the visit stack?
  for (i = vstk_base; i < defs->len; i++) {
    if UNLIKELY(defs->v[i] == (node_t*)bt)
      return error_ownership_cycle(c, defs, vstk_base, bt, origin);
  }

  // has bt been checked already? (not in defs anymore)
  for (i = 0; i < vstk_base; i++) {
    if UNLIKELY(defs->v[i] == (node_t*)bt) {
      defs->v[i] = NULL;
      break;
    }
  }

  // we are done if we have already visited bt
  if (i == vstk_base)
    return true;

  // push entry onto visited stack
  safecheckxf(nodearray_push(defs, c->ma, (node_t*)bt), "OOM");

  bool ok = true;

  switch (bt->kind) {

  case TYPE_ARRAY:
  case TYPE_REF:
  case TYPE_MUTREF:
  case TYPE_SLICE:
  case TYPE_MUTSLICE:
    ok = check_type(c, defs, vstk_base, aliasnest, ((ptrtype_t*)bt)->elem, bt);
    break;

  case TYPE_ALIAS: {
    // check for special case of alias of array of same alias, e.g.
    //   type A [&A]
    const aliastype_t* at = (aliastype_t*)bt;
    if UNLIKELY(
      at->elem->kind == TYPE_ARRAY &&
      type_unwrap_ptr(((arraytype_t*)at->elem)->elem) == bt )
    {
      safecheckxf(nodearray_push(defs, c->ma, (node_t*)at->elem), "OOM");
      defs->v[defs->len-1] = defs->v[defs->len-2];
      defs->v[defs->len-2] = (node_t*)at->elem;
      error_ownership_cycle(c, defs, vstk_base, at->elem, origin);
      error_ownership_cycle_help(c, bt, origin);
      return false;
    }
    ok = check_type(c, defs, vstk_base, aliasnest+1, ((ptrtype_t*)bt)->elem, bt);
    break;
  }

  case TYPE_STRUCT:
    for (u32 i = 0; i < ((structtype_t*)bt)->fields.len; i++) {
      const local_t* field = (local_t*)((structtype_t*)bt)->fields.v[i];
      // Note: we could allow optional owning pointers here, e.g. "type T { x ?*T }"
      // by checking for field.type==opt && field.type.elem==ptr and "continue"ing.
      // However, allowing that would require updating ownership code generation in cgen
      // to handle this case, which is non-trivial. Since cycles like these can be long,
      // e.g. A B C A, "type A { x ?*B }; type B { x ?*C }; type C { x ?*A }",
      // the code we would need to generate to drop such a type like A B or C is complex.
      if (!check_type(c, defs, vstk_base, aliasnest, field->type, field)) {
        ok = false;
        break;
      }
    }
    break;

  case TYPE_TEMPLATE: {
    templatetype_t* tt = (templatetype_t*)bt;
    type_t* recvt = (type_t*)tt->recv;
    if (!check_type(c, defs, vstk_base, aliasnest, recvt, bt)) {
      ok = false;
      break;
    }
    for (u32 i = 0; i < tt->args.len; i++) {
      const type_t* arg = (type_t*)tt->args.v[i];
      assertf(nodekind_istype(arg->kind), "%s", nodekind_name(arg->kind));
      if (!check_type(c, defs, vstk_base, aliasnest, arg, bt)) {
        ok = false;
        break;
      }
    }
    break;
  }

  default:
    assertf(0, "unexpected %s", nodekind_name(bt->kind));
    UNREACHABLE;
    ok = false;
  }

  // pop entry from visited stack
  defs->len--;

  if UNLIKELY(!ok)
    error_ownership_cycle_help(c, bt, origin);

  return ok;
}


bool check_typedep(compiler_t* c, node_t* n) {
  bool ok = false;
  nodearray_t defs = {0};
  u32 visitflags = 0;
  if (ast_toposort_visit_def(&defs, c->ma, 0, n, visitflags)) {
    u32 vstk_base = defs.len;
    ok = true;
    for (u32 i = 0; i < defs.len && ok; i++) {
      if (defs.v[i] == NULL || !node_istype(defs.v[i]))
        continue;
      ok = check_type(c, &defs, vstk_base, 0, (type_t*)defs.v[i], NULL);
    }
  }
  nodearray_dispose(&defs, c->ma);
  return ok;
}


err_t check_typedeps(compiler_t* c, unit_t** unitv, u32 unitc) {
  err_t err = 0;
  nodearray_t defs = {0};
  u32 visitflags = 0;

  // collect all unique definitions in a topologically sorted array
  for (u32 i = 0; i < unitc; i++) {
    const nodearray_t children = unitv[i]->children;
    for (u32 i = 0; i < children.len; i++) {
      if (!ast_toposort_visit_def(&defs, c->ma, 0, children.v[i], visitflags)) {
        err = ErrNoMem;
        goto end;
      }
    }
  }

  // use rest of defs array for "visited" stack
  u32 vstk_base = defs.len;

  // defs are sorted from "least to most dependencies", e.g.
  //   type X { x int }      // 0 dependencies
  //   type Y { x int }      // 0 dependencies
  //   type A { x X }        // 1 dependency
  //   type B { x X; y Y }   // 2 dependencies
  //
  for (u32 i = 0; i < defs.len; i++) {
    if (defs.v[i] == NULL || !node_istype(defs.v[i]))
      continue;
    if (!check_type(c, &defs, vstk_base, 0, (type_t*)defs.v[i], NULL))
      break;
  }

end:
  nodearray_dispose(&defs, c->ma);
  return err;
}
