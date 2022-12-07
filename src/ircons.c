// SPDX-License-Identifier: Apache-2.0
#include "c0lib.h"
#include "compiler.h"

#define TRACE_ANALYSIS


typedef struct {
  irfun_t*   f;
  irblock_t* b;
} fstate_t;

DEF_ARRAY_TYPE(fstate_t, fstatearray)
DEF_ARRAY_TYPE(map_t, maparray)


typedef struct {
  compiler_t*     compiler;
  memalloc_t      ma;          // compiler->ma
  memalloc_t      ir_ma;       // allocator for ir data
  irunit_t*       unit;        // current unit
  irfun_t*        f;           // current function
  irblock_t*      b;           // current block
  fstatearray_t   fstack;      // suspended function builds
  err_t           err;         // result of build process
  buf_t           tmpbuf[2];   // general-purpose scratch buffers
  map_t           funm;        // {fun_t* => irfun_t*} for breaking cycles
  map_t           vars;        // {sym_t => irval_t*} (moved to defvars by end_block)
  maparray_t      defvars;     // {[block_id] => map_t}
  maparray_t      pendingphis; // {[block_id] => map_t}
  maparray_t      freemaps;    // free map_t's (for defvars and pendingphis)

  #ifdef TRACE_ANALYSIS
    int traceindent;
  #endif
} ircons_t;


static irval_t   bad_irval = { 0 };
static irblock_t bad_irblock = { 0 };
static funtype_t bad_astfuntype = { .kind = TYPE_FUN };
static fun_t     bad_astfun = { .kind = EXPR_FUN, .type = (type_t*)&bad_astfuntype };
static irfun_t   bad_irfun = { .ast = &bad_astfun };
static irunit_t  bad_irunit = { 0 };


ATTR_FORMAT(printf, 3, 4)
static const char* fmttmp(ircons_t* c, u32 bufidx, const char* fmt, ...) {
  buf_t* buf = &c->tmpbuf[bufidx];
  buf_clear(buf);
  va_list ap;
  va_start(ap, fmt);
  buf_vprintf(buf, fmt, ap);
  va_end(ap);
  return buf->chars;
}


static const char* fmtnodex(ircons_t* c, u32 bufidx, const void* nullable n, u32 depth) {
  buf_t* buf = &c->tmpbuf[bufidx];
  buf_clear(buf);
  node_fmt(buf, n, depth);
  return buf->chars;
}

// const char* fmtnode(u32 bufidx, const void* nullable n)
#define fmtnode(bufidx, n) fmtnodex(c, (bufidx), (n), 0)


#ifdef TRACE_ANALYSIS
  #define trace(fmt, va...)  \
    _dlog(2, "A", __FILE__, __LINE__, "%*s" fmt, c->traceindent*2, "", ##va)
  static void trace_node(ircons_t* c, const char* msg, const node_t* n) {
    trace("%s%-14s: %s", msg, nodekind_name(n->kind), fmtnode(0, n));
  }
  static void traceindent_decr(void* cp) {
    (*(ircons_t**)cp)->traceindent--;
  }
#else
  #define trace(fmt, va...) ((void)0)
  #define trace_node(a,msg,n) ((void)0)
#endif


static void assert_types_iscompat(ircons_t* c, const type_t* x, const type_t* y) {
  assertf(types_iscompat(x, y), "%s != %s", fmtnode(0, x), fmtnode(1, y));
}


static void seterr(ircons_t* c, err_t err) {
  if (!c->err)
    c->err = err;
}


static void out_of_mem(ircons_t* c) {
  seterr(c, ErrNoMem);
}


static bool alloc_map(ircons_t* c, map_t* dst) {
  if (c->freemaps.len) {
    *dst = c->freemaps.v[c->freemaps.len - 1];
    c->freemaps.len--;
    return true;
  }
  return map_init(dst, c->ma, 8);
}


static void free_map(ircons_t* c, map_t* m) {
  map_t* free_slot = maparray_alloc(&c->freemaps, c->ma, 1);
  if UNLIKELY(!free_slot) {
    map_dispose(m, c->ma);
  } else {
    map_clear(m);
    *free_slot = *m;
    m->cap = 0;
  }
}


static map_t* nullable get_block_map(maparray_t* a, u32 block_id) {
  return (a->len > block_id && a->v[block_id].len != 0) ? &a->v[block_id] : NULL;
}


static void del_block_map(ircons_t* c, maparray_t* a, u32 block_id) {
  assertf(get_block_map(a, block_id), "no block map for b%u", block_id);
  free_map(c, &a->v[block_id]);
}


static map_t* nullable assign_block_map(ircons_t* c, maparray_t* a, u32 block_id) {
  if (a->len <= block_id) {
    // fill holes
    u32 nholes = (block_id - a->len) + 1;
    map_t* p = maparray_alloc(&c->defvars, c->ma, nholes);
    if UNLIKELY(!p)
      return out_of_mem(c), NULL;
    memset(p, 0, sizeof(map_t) * nholes);
  }
  if (a->v[block_id].cap == 0) {
    if UNLIKELY(!alloc_map(c, &a->v[block_id]))
      out_of_mem(c);
  }
  return &a->v[block_id];
}


#define comment(c, obj, comment) _Generic((obj), \
  irval_t*:   val_comment, \
  irblock_t*: block_comment )((c), (void*)(obj), (comment))

static irval_t* val_comment(ircons_t* c, irval_t* v, const char* comment) {
  v->comment = mem_strdup(c->ir_ma, slice_cstr(comment), 0);
  return v;
}

static irblock_t* block_comment(ircons_t* c, irblock_t* b, const char* comment) {
  b->comment = mem_strdup(c->ir_ma, slice_cstr(comment), 0);
  return b;
}


static irval_t* mkval(ircons_t* c, op_t op, srcloc_t loc, type_t* type) {
  irval_t* v = mem_alloct(c->ir_ma, irval_t);
  if UNLIKELY(v == NULL)
    return out_of_mem(c), &bad_irval;
  v->id = c->f->vidgen++;
  v->op = op;
  v->loc = loc;
  v->type = type;
  return v;
}


static irval_t* pushval(ircons_t* c, irblock_t* b, op_t op, srcloc_t loc, type_t* type) {
  irval_t* v = mkval(c, op, loc, type);
  if UNLIKELY(!ptrarray_push(&b->values, c->ir_ma, v))
    out_of_mem(c);
  return v;
}


static irval_t* insertval(
  ircons_t* c, irblock_t* b, u32 at_index, op_t op, srcloc_t loc, type_t* type)
{
  irval_t* v = mkval(c, op, loc, type);
  irval_t** vp = (irval_t**)ptrarray_allocat(&b->values, c->ir_ma, at_index, 1);
  if UNLIKELY(!vp)
    return out_of_mem(c), v;
  *vp = v;
  return v;
}


#define push_TODO_val(c, b, fmt, args...) ( \
  dlog("TODO_val " fmt, ##args), \
  comment((c), pushval((c), (b), OP_NOOP, (srcloc_t){0}, type_void), "TODO") \
)


static void pusharg(irval_t* dst, irval_t* arg) {
  assert(dst->argc < countof(dst->argv));
  dst->argv[dst->argc++] = arg;
  arg->nuse++;
}


static irblock_t* mkblock(ircons_t* c, irfun_t* f, irblockkind_t kind, srcloc_t loc) {
  irblock_t* b = mem_alloct(c->ir_ma, irblock_t);
  if UNLIKELY(b == NULL || !ptrarray_push(&f->blocks, c->ir_ma, b))
    return out_of_mem(c), &bad_irblock;
  b->id = f->bidgen++;
  b->kind = kind;
  b->loc = loc;
  return b;
}


inline static u32 npreds(const irblock_t* b) {
  static_assert(countof(b->preds) == 2, "countof(irblock_t.preds) changed");
  assertf( (b->preds[1] == NULL && b->preds[0]) || b->preds[0],
    "has preds[1] (b%u) but no b->preds[0]", b->preds[1]->id);
  return (u32)!!b->preds[0] + (u32)!!b->preds[1];
}


//—————————————————————————————————————————————————————————————————————————————————————


static irval_t* var_read_recursive(ircons_t*, irblock_t*, sym_t, type_t*, srcloc_t);


static void var_write_map(ircons_t* c, map_t* vars, sym_t name, irval_t* v) {
  irval_t** vp = (irval_t**)map_assign_ptr(vars, c->ma, name);
  if UNLIKELY(!vp)
    return out_of_mem(c);
  if (*vp) trace("  replacing v%u", (*vp)->id);
  *vp = v;
}


static irval_t* var_read_map(
  ircons_t* c, irblock_t* b, map_t* vars, sym_t name, type_t* type, srcloc_t loc)
{
  irval_t** vp = (irval_t**)map_lookup_ptr(vars, name);
  if (vp)
    return assertnotnull(*vp);
  trace("%s %s not found in b%u", __FUNCTION__, name, b->id);
  return var_read_recursive(c, b, name, type, loc);
}


static void var_write_inblock(ircons_t* c, irblock_t* b, sym_t name, irval_t* v) {
  trace("%s %s = v%u (b%u)", __FUNCTION__, name, v->id, b->id);
  map_t* vars;
  if (b == c->b) {
    vars = &c->vars;
  } else {
    vars = assign_block_map(c, &c->defvars, b->id);
  }
  var_write_map(c, vars, name, v);
}


static irval_t* var_read_inblock(
  ircons_t* c, irblock_t* b, sym_t name, type_t* type, srcloc_t loc)
{
  trace("%s %s in b%u", __FUNCTION__, name, b->id);
  map_t* vars = assign_block_map(c, &c->defvars, b->id);
  return var_read_map(c, b, vars, name, type, loc);
}


static void var_write(ircons_t* c, sym_t name, irval_t* v) {
  trace("%s %s = v%u", __FUNCTION__, name, v->id);
  var_write_map(c, &c->vars, name, v);
}


static irval_t* var_read(ircons_t* c, sym_t name, type_t* type, srcloc_t loc) {
  trace("%s %s", __FUNCTION__, name);
  return var_read_map(c, c->b, &c->vars, name, type, loc);
}


static void add_pending_phi(ircons_t* c, irblock_t* b, irval_t* phi, sym_t name) {
  // tracks pending, incomplete phis that are completed by seal_block for
  // blocks that are sealed after they have started. This happens when preds
  // are not known at the time a block starts, but is known and registered
  // before the block ends.
  trace("%s in b%u for %s", __FUNCTION__, b->id, name);
  map_t* phimap = assign_block_map(c, &c->pendingphis, b->id);
  if UNLIKELY(!phimap)
    return;
  void** vp = map_assign_ptr(phimap, c->ma, name);
  if UNLIKELY(!vp)
    return out_of_mem(c);
  assertf(*vp == NULL, "duplicate phi for %s", name);
  phi->aux.ptr = b;
  *vp = phi;
}


static irval_t* var_read_recursive(
  ircons_t* c, irblock_t* b, sym_t name, type_t* type, srcloc_t loc)
{
  // global value numbering
  trace("%s %s in b%u", __FUNCTION__, name, b->id);

  irval_t* v;

  if ((b->flags & IR_SEALED) == 0) {
    // incomplete CFG
    trace("  block not yet sealed");
    v = pushval(c, b, OP_PHI, loc, type);
    add_pending_phi(c, b, v, name);
  } else if (npreds(b) == 1) {
    trace("  read in single predecessor b%u", b->preds[0]->id);
    // optimize the common case of single predecessor; no phi needed
    v = var_read_inblock(c, b->preds[0], name, type, loc);
  } else if (npreds(b) == 0) {
    // outside of function
    trace("  outside of function (no predecessors)");
    // ... read_global(c, ...)
    v = push_TODO_val(c, b, "gvn");
  } else {
    // multiple predecessors
    trace("  read in predecessors b%u, b%u", b->preds[0]->id, b->preds[1]->id);
    v = pushval(c, b, OP_PHI, loc, type);
    var_write_inblock(c, b, name, v);
    assert(npreds(b) == 2);
    pusharg(v, var_read_inblock(c, b->preds[0], name, type, loc));
    pusharg(v, var_read_inblock(c, b->preds[1], name, type, loc));
    return v;
  }

  var_write_inblock(c, b, name, v);
  return v;
}


//—————————————————————————————————————————————————————————————————————————————————————


static void set_control(irblock_t* b, irval_t* nullable v) {
  if (v)
    v->nuse++;
  if (b->control)
    b->control->nuse--;
  b->control = v;
}


static void seal_block(ircons_t* c, irblock_t* b) {
  // sets IR_SEALED, indicating that no further predecessors will be added (to b.preds)
  trace("%s b%u", __FUNCTION__, b->id);
  assert((b->flags & IR_SEALED) == 0);
  b->flags |= IR_SEALED;

  map_t* pendingphis = get_block_map(&c->pendingphis, b->id);
  if (pendingphis) {
    trace("flush pendingphis for b%u", b->id);
    for (const mapent_t* e = map_it(pendingphis); map_itnext(pendingphis, &e); ) {
      sym_t name = e->key;
      irval_t* v = e->value;
      trace("  pendingphis['%s'] => v%u", name, v->id);
      irblock_t* b = v->aux.ptr;
      assertf(b, "block was not stored in phi->aux.ptr");
      pusharg(v, var_read_inblock(c, b->preds[0], name, v->type, v->loc));
      pusharg(v, var_read_inblock(c, b->preds[1], name, v->type, v->loc));
    }
    del_block_map(c, &c->pendingphis, b->id);
  }
}


static void start_block(ircons_t* c, irblock_t* b) {
  trace("%s b%u", __FUNCTION__, b->id);
  assertf(c->b == &bad_irblock, "maybe forgot to call end_block?");
  c->b = b;
}


static void stash_block_vars(ircons_t* c, irblock_t* b) {
  // moves block-local vars to long-term definition data
  if (c->vars.len == 0)
    return;

  // save vars
  trace("stash %u var%s modified by b%u", c->vars.len, c->vars.len==1?"":"s", b->id);
  // for (const mapent_t* e = map_it(&c->vars); map_itnext(&c->vars, &e); )
  //   trace("  '%s' => v%u", (const char*)e->key, ((irval_t*)e->value)->id);

  map_t* vars = assign_block_map(c, &c->defvars, b->id);
  if UNLIKELY(!vars)
    return;
  *vars = c->vars;

  // replace c.vars with a new map
  if (!map_init(&c->vars, c->ma, 8))
    return out_of_mem(c);
}


static irblock_t* end_block(ircons_t* c) {
  // transfer live locals and seals c->b if needed
  trace("%s b%u", __FUNCTION__, c->b->id);

  irblock_t* b = c->b;
  c->b = &bad_irblock;
  assertf(b != &bad_irblock, "unbalanced start_block/end_block");

  stash_block_vars(c, b);

  if (!(b->flags & IR_SEALED)) {
    seal_block(c, b);
  } else {
    assertf(get_block_map(&c->pendingphis, b->id) == NULL,
      "sealed block with pending PHIs");
  }

  return b;
}


static void discard_block(ircons_t* c, irblock_t* b) {
  ptrarray_t* blocks = &c->f->blocks;

  // make sure there are no cfg edges to this block
  #if DEBUG
  for (u32 i = 0; i < blocks->len; i++) {
    irblock_t* b2 = blocks->v[i];
    if (b2 == b)
      continue;
    assertf(b2->preds[0] != b, "b%u references b%u (preds[0])", b2->id, b->id);
    assertf(b2->preds[1] != b, "b%u references b%u (preds[1])", b2->id, b->id);
    assertf(b2->succs[0] != b, "b%u references b%u (succs[0])", b2->id, b->id);
    assertf(b2->succs[1] != b, "b%u references b%u (succs[1])", b2->id, b->id);
  }
  #endif

  if (blocks->v[blocks->len - 1] == b) {
    blocks->len--;
    return;
  }
  u32 i = 0;
  for (; i < blocks->len; i++) {
    if (blocks->v[i] == b)
      break;
  }
  assertf(i < blocks->len, "b%u not in current function", b->id);
  ptrarray_remove(blocks, i, 1);
}


static irblock_t* entry_block(irfun_t* f) {
  assert(f->blocks.len > 0);
  return f->blocks.v[0];
}


//—————————————————————————————————————————————————————————————————————————————————————


static irval_t* expr(ircons_t* c, expr_t* n);


static irval_t* idexpr(ircons_t* c, idexpr_t* n) {
  assertnotnull(n->ref);
  assertf(node_islocal(n->ref), "%s", nodekind_name(n->ref->kind));
  local_t* local = (local_t*)n->ref;
  return var_read(c, local->name, local->type, local->loc);
}


static irval_t* local(ircons_t* c, local_t* n) {
  irval_t* init;
  if (n->init) {
    init = expr(c, n->init);
  } else {
    init = pushval(c, c->b, OP_ZERO, n->loc, n->type);
  }

  if (n->name == sym__)
    return init;

  var_write(c, n->name, init);

  if (n->kind == EXPR_LET)
    return init;

  // irval_t* mem = pushval(c, c->b, OP_LOCAL, n->loc, n->type);
  // comment(c, mem, n->name);
  // irval_t* store = pushval(c, c->b, OP_STORE, n->loc, n->type);
  // pusharg(store, mem);
  // pusharg(store, init);

  return init;
}


static irval_t* assign(ircons_t* c, binop_t* n) {
  irval_t* value = expr(c, n->right);

  idexpr_t* id = (idexpr_t*)n->left; assert(id->kind == EXPR_ID);
  if (id->name == sym__)
    return value;

  if (!node_islocal(id->ref))
    return push_TODO_val(c, c->b, "%s", nodekind_name(id->ref->kind));

  local_t* local = (local_t*)id->ref;

  // symbolic, never used
  irval_t* v = pushval(c, c->b, OP_STORE, n->loc, n->type);
  pusharg(v, value);

  var_write(c, local->name, value);

  return value;
}


static irval_t* binop(ircons_t* c, binop_t* n) {
  irval_t* left  = expr(c, n->left);
  irval_t* right = expr(c, n->right);
  assert(left->type && right->type);
  assert_types_iscompat(c, left->type, right->type);
  irval_t* v = pushval(c, c->b, n->op, n->loc, n->type);
  pusharg(v, left);
  pusharg(v, right);
  return v;
}


static irval_t* intlit(ircons_t* c, intlit_t* n) {
  // "intern" constants
  // this is a really simple solution:
  // - all constants are placed at the beginning of the entry block
  //   - first int constants, then float constants
  // - linear scan for an existing equivalent constant
  // - fast for functions with few constants, which is the common case
  // - degrades for functions with many constants
  //   - could do binary search if we bookkeep ending index
  irblock_t* b0 = entry_block(c->f);
  u64 intval = n->intval;
  u32 i = 0;
  for (; i < b0->values.len; i++) {
    irval_t* v = b0->values.v[i];
    if (v->op != OP_ICONST || v->aux.i64val > intval)
      break;
    if (v->aux.i64val == intval && v->type == n->type)
      return v;
  }
  irval_t* v = insertval(c, b0, i, OP_ICONST, n->loc, n->type);
  v->aux.i64val = intval;
  return v;
}


static irval_t* floatlit(ircons_t* c, floatlit_t* n) {
  irblock_t* b0 = entry_block(c->f);
  f64 f64val = n->f64val;
  u32 i = 0;
  for (; i < b0->values.len; i++) {
    irval_t* v = b0->values.v[i];
    if (v->op == OP_ICONST)
      continue;
    if (v->op != OP_FCONST || v->aux.f64val > f64val)
      break;
    if (v->aux.f64val == f64val && v->type == n->type)
      return v;
  }
  irval_t* v = insertval(c, b0, i, OP_FCONST, n->loc, n->type);
  v->aux.f64val = f64val;
  return v;
}


static irval_t* retexpr(ircons_t* c, retexpr_t* n) {
  irval_t* v = n->value ? expr(c, n->value) : NULL;
  c->b->kind = IR_BLOCK_RET;
  set_control(c->b, v);
  return v ? v : &bad_irval;
}


static irval_t* blockexpr(ircons_t* c, block_t* n) {
  irval_t* v = &bad_irval;
  for (u32 i = 0; i < n->children.len; i++) {
    expr_t* cn = n->children.v[i];
    v = expr(c, cn);
    if (cn->kind == EXPR_RETURN)
      break;
  }
  return v;
}


#ifdef TRACE_ANALYSIS
  #define blockexpr1(c, n)  expr((c), (expr_t*)(n))
#else
  #define blockexpr1(c, n)  blockexpr((c), (expr_t*)(n))
#endif


static irval_t* ifexpr(ircons_t* c, ifexpr_t* n) {
  // if..end has the following semantics:
  //
  //   if cond b1 b2
  //   b1:
  //     <then-block>
  //   goto b2
  //   b2:
  //     <continuation-block>
  //
  // if..else..end has the following semantics:
  //
  //   if cond b1 b2
  //   b1:
  //     <then-block>
  //   goto b3
  //   b2:
  //     <else-block>
  //   goto b3
  //   b3:
  //     <continuation-block>
  irfun_t* f = c->f;

  // generate control condition
  irval_t* control = expr(c, n->cond);
  assert(control->type == type_bool);

  // end predecessor block (leading up to and including "if")
  irblock_t* ifb = end_block(c);
  ifb->kind = IR_BLOCK_IF;
  set_control(ifb, control);

  // create blocks for then and else branches
  irblock_t* thenb = mkblock(c, f, IR_BLOCK_CONT, n->thenb->loc);
  irblock_t* elseb = mkblock(c, f, IR_BLOCK_CONT, n->elseb ? n->elseb->loc : n->loc);
  u32 elseb_index = f->blocks.len - 1; // used later for moving blocks
  ifb->succs[0] = thenb;
  ifb->succs[1] = elseb; // if -> then, else
  comment(c, thenb, fmttmp(c, 0, "b%u.then", ifb->id));

  // begin "then" block
  trace("if \"then\" block");
  thenb->preds[0] = ifb; // then <- if
  start_block(c, thenb);
  seal_block(c, thenb);
  irval_t* thenv = blockexpr1(c, n->thenb);
  thenb = end_block(c);

  irval_t* elsev;

  // begin "else" block if there is one
  if (n->elseb) {
    trace("if \"else\" block");

    // allocate "cont" block; the block following both thenb and elseb
    u32 contb_index = f->blocks.len;
    irblock_t* contb = mkblock(c, f, IR_BLOCK_CONT, n->loc);
    comment(c, contb, fmttmp(c, 0, "b%u.cont", ifb->id));

    // begin "else" block
    comment(c, elseb, fmttmp(c, 0, "b%u.else", ifb->id));
    elseb->preds[0] = ifb; // else <- if
    start_block(c, elseb);
    seal_block(c, elseb);
    elsev = blockexpr1(c, n->elseb);

    // if "then" block returns, no "cont" block needed
    if (thenb->kind == IR_BLOCK_RET) {
      discard_block(c, contb);
      return elsev;
    }

    elseb = end_block(c);

    // wire up graph edges
    elseb->succs[0] = contb; // else -> cont
    thenb->succs[0] = contb; // then -> cont
    if (thenb->kind == IR_BLOCK_RET) {
      contb->preds[0] = elseb; // cont <- else
    } else if (elseb->kind == IR_BLOCK_RET) {
      contb->preds[0] = thenb; // cont <- then
    } else {
      contb->preds[0] = thenb; // ...
      contb->preds[1] = elseb; // cont <- then, else
    }

    // begin "cont" block
    start_block(c, contb);
    seal_block(c, contb);

    // move cont block to end (in case blocks were created by "else" body)
    ptrarray_move(&f->blocks, f->blocks.len - 1, contb_index, contb_index + 1);

    // sanity check types
    assertf(types_iscompat(thenv->type, elsev->type),
      "branch type mismatch %s, %s",
      fmtnode(0, thenv->type), fmtnode(1, elsev->type));

    if (elseb->values.len == 0) {
      // "else" body may be empty in case it refers to an existing value. For example:
      //   x = 9 ; y = if true { x + 1 } else { x }
      // This compiles to:
      //   b0:
      //     v1 = const 9
      //     v2 = const 1
      //   if true -> b1, b2
      //   b1:
      //     v3 = add v1 v2
      //   cont -> b3
      //   b2:                    #<-  Note: Empty
      //   cont -> b3
      //   b3:
      //     v4 = phi v3 v1
      //
      // The above can be reduced to:
      //   b0:
      //     v1 = const 9
      //     v2 = const 1
      //   if true -> b1, b3     #<- change elseb to contb
      //   b1:
      //     v3 = add v1 v2
      //   cont -> b3
      //                         #<- remove elseb
      //   b3:
      //     v4 = phi v3 v1      #<- phi remains valid; no change needed
      //
      ifb->succs[1] = contb;  // if -> cont
      contb->preds[1] = ifb;  // cont <- if
      discard_block(c, elseb);
      elseb = NULL;
    }
  } else {
    // no "else" block; convert elseb to "end" block
    comment(c, elseb, fmttmp(c, 0, "b%u.cont", ifb->id));
    thenb->succs[0] = elseb; // then -> else
    elseb->preds[0] = ifb;
    if (thenb->kind != IR_BLOCK_RET)
      elseb->preds[1] = thenb; // else <- if, then
    start_block(c, elseb);
    seal_block(c, elseb);

    // move cont block to end (in case blocks were created by "then" body)
    ptrarray_move(&f->blocks, f->blocks.len - 1, elseb_index, elseb_index + 1);

    if (n->flags & EX_RVALUE) {
      // zero in place of "else" block
      elsev = pushval(c, c->b, OP_ZERO, n->loc, thenv->type);
    } else {
      elsev = thenv;
    }

    // if "then" block returns, no PHI is needed
    if (thenb->kind == IR_BLOCK_RET)
      return elsev;
  }

  // if "else" block returns, or the result of the "if" is not used, so no PHI needed.
  // e.g. "fun f(x int) { 2 * if x > 0 { x - 2 } else { return x + 2 } }"
  //   b0:
  //     v0 = arg 0
  //     v1 = const 0
  //     v2 = const 2
  //     v3 = gt v0 v1   # x > 0
  //   if v3 -> b1, b2
  //   b1:               # then
  //     v4 = sub v0 v2  # x - 2
  //   cont -> b3
  //   b2:               # else
  //     v5 = add v0 v1  # x + 2
  //   ret v5            # return
  //   b3:
  //     v6 = mul v2 v5  # <- note: only v5 gets here, not v4
  //   ret v6
  if (elseb->kind == IR_BLOCK_RET || (n->flags & EX_RVALUE) == 0)
    return thenv;

  // make Phi, joining the two branches together
  assertf(c->b->preds[0], "phi in block without predecessors");
  irval_t* phi = pushval(c, c->b, OP_PHI, n->loc, thenv->type);
  pusharg(phi, thenv);
  pusharg(phi, elsev);
  comment(c, phi, "if");
  return phi;
}


static irfun_t* fun(ircons_t* c, fun_t* n) {
  // functions may refer to themselves, so we record "ongoing" functions in a map
  irfun_t** fp = (irfun_t**)map_assign_ptr(&c->funm, c->ma, n);
  if UNLIKELY(!fp)
    return out_of_mem(c), &bad_irfun;
  if (*fp) {
    // fun already built or in progress of being built
    return *fp;
  }

  // allocate irfun_t
  irfun_t* f = mem_alloct(c->ir_ma, irfun_t);
  if UNLIKELY(!f)
    return out_of_mem(c), &bad_irfun;
  *fp = f;
  f->name = mem_strdup(c->ir_ma, slice_cstr(n->name), 0);
  f->ast = n;

  // add to current unit
  if UNLIKELY(!ptrarray_push(&c->unit->functions, c->ir_ma, f)) {
    out_of_mem(c);
    goto end;
  }

  // save current function build state
  if (c->f != &bad_irfun) {
    // TODO: a better strategy would be to use a queue of functions:
    // - at the unit level:
    //   - initialize with all functions in the unit
    //   - while queue is not empty:
    //     - deque first and generate function
    // - when encountering a local function, prepend it to the queue
    //
    // This approach would save us from this complicated state saving stuff.
    //
    fstate_t* fstate = fstatearray_alloc(&c->fstack, c->ma, 1);
    if UNLIKELY(!fstate) {
      out_of_mem(c);
      goto end;
    }
    fstate->f = c->f;
    fstate->b = c->b;
    c->b = &bad_irblock; // satisfy assertion in start_block
    panic("TODO save vars, defvars and pendingphis");
  }

  c->f = f;

  // allocate entry block
  irblock_t* entryb = mkblock(c, f, IR_BLOCK_CONT, n->loc);
  start_block(c, entryb);
  seal_block(c, entryb); // entry block has no predecessors

  // define arguments
  for (u32 i = 0; i < n->params.len; i++) {
    local_t* param = n->params.v[i];
    if (param->name == sym__)
      continue;
    irval_t* v = pushval(c, c->b, OP_ARG, param->loc, param->type);
    v->aux.i32val = i;
    comment(c, v, param->name);
    var_write(c, param->name, v);
  }

  // build body
  irval_t* body = blockexpr1(c, (expr_t*)n->body);

  // end last block, if not already ended
  c->b->kind = IR_BLOCK_RET;
  if (((funtype_t*)n->type)->result != type_void)
    set_control(c->b, body);
  end_block(c);

  // reset
  map_clear(&c->vars);
  for (u32 i = 0; i < c->defvars.len; i++) {
    if (c->defvars.v[i].cap)
      free_map(c, &c->defvars.v[i]);
  }
  for (u32 i = 0; i < c->pendingphis.len; i++) {
    if (c->pendingphis.v[i].cap)
      free_map(c, &c->pendingphis.v[i]);
  }

end:

  // restore past function build state
  if (c->fstack.len > 0) {
    c->f = c->fstack.v[c->fstack.len - 1].f;
    c->b = c->fstack.v[c->fstack.len - 1].b;
    fstatearray_pop(&c->fstack);
  } else {
    c->f = &bad_irfun;
  }

  return f;
}


static irval_t* expr(ircons_t* c, expr_t* n) {
  #ifdef TRACE_ANALYSIS
    trace_node(c, "expr ", (node_t*)n);
    c->traceindent++;
    ircons_t* c2 __attribute__((__cleanup__(traceindent_decr),__unused__)) = c;
  #endif

  switch ((enum nodekind)n->kind) {
  case EXPR_BLOCK:     return blockexpr(c, (block_t*)n);
  case EXPR_BINOP:     return binop(c, (binop_t*)n);
  case EXPR_ASSIGN:    return assign(c, (binop_t*)n);
  case EXPR_ID:        return idexpr(c, (idexpr_t*)n);
  case EXPR_RETURN:    return retexpr(c, (retexpr_t*)n);
  case EXPR_FLOATLIT:  return floatlit(c, (floatlit_t*)n);
  case EXPR_IF:        return ifexpr(c, (ifexpr_t*)n);

  case EXPR_BOOLLIT:
  case EXPR_INTLIT:
    return intlit(c, (intlit_t*)n);

  case EXPR_FIELD:
  case EXPR_PARAM:
  case EXPR_VAR:
  case EXPR_LET:
    return local(c, (local_t*)n);

  // TODO
  case EXPR_FUN:       // return funexpr(c, (fun_t*)n);
  case EXPR_CALL:      // return call(c, (call_t*)n);
  case EXPR_MEMBER:    // return member(c, (member_t*)n);
  case EXPR_DEREF:     // return deref(c, (unaryop_t*)n);
  case EXPR_PREFIXOP:  // return prefixop(c, (unaryop_t*)n);
  case EXPR_POSTFIXOP: // return postfixop(c, (unaryop_t*)n);
  case EXPR_FOR:
    c->err = ErrCanceled;
    return push_TODO_val(c, c->b, "expr(%s)", nodekind_name(n->kind));

  // We should never see these kinds of nodes
  case NODEKIND_COUNT:
  case NODE_BAD:
  case NODE_COMMENT:
  case NODE_UNIT:
  case STMT_TYPEDEF:
  case TYPE_VOID:
  case TYPE_BOOL:
  case TYPE_INT:
  case TYPE_I8:
  case TYPE_I16:
  case TYPE_I32:
  case TYPE_I64:
  case TYPE_F32:
  case TYPE_F64:
  case TYPE_ARRAY:
  case TYPE_ENUM:
  case TYPE_FUN:
  case TYPE_PTR:
  case TYPE_REF:
  case TYPE_OPTIONAL:
  case TYPE_STRUCT:
    break;
  }
  assertf(0, "unexpected node %s", nodekind_name(n->kind));
}


static irunit_t* unit(ircons_t* c, unit_t* n) {
  irunit_t* u = mem_alloct(c->ir_ma, irunit_t);
  if UNLIKELY(!u)
    return out_of_mem(c), &bad_irunit;

  assert(c->unit == &bad_irunit);
  c->unit = u;

  for (u32 i = 0; i < n->children.len; i++) {
    stmt_t* cn = n->children.v[i];

    #ifdef TRACE_ANALYSIS
      trace_node(c, "stmt ", (node_t*)cn);
      c->traceindent++;
      ircons_t* c2 __attribute__((__cleanup__(traceindent_decr),__unused__)) = c;
    #endif

    switch (cn->kind) {
      case STMT_TYPEDEF:
        // ignore
        break;
      case EXPR_FUN:
        fun(c, (fun_t*)cn);
        break;
      default:
        assertf(0, "unexpected node %s", nodekind_name(cn->kind));
    }
  }

  c->unit = &bad_irunit;

  return u;
}


static void dispose_maparray(memalloc_t ma, maparray_t* a) {
  for (u32 i = 0; i < a->len; i++) {
    if (a->v[i].cap)
      map_dispose(&a->v[i], ma);
  }
  maparray_dispose(a, ma);
}


static err_t ircons(
  compiler_t* compiler, memalloc_t ir_ma, unit_t* n, irunit_t** result)
{
  ircons_t c = {
    .compiler = compiler,
    .ma = compiler->ma,
    .ir_ma = ir_ma,
    .unit = &bad_irunit,
    .f = &bad_irfun,
    .b = &bad_irblock,
  };

  bad_irval.type = type_void;
  bad_astfuntype.result = type_void;

  if (!map_init(&c.funm, c.ma, n->children.len*2)) {
    c.err = ErrNoMem;
    goto end1;
  }
  if (!map_init(&c.vars, c.ma, 8)) {
    c.err = ErrNoMem;
    goto end2;
  }

  for (usize i = 0; i < countof(c.tmpbuf); i++)
    buf_init(&c.tmpbuf[i], c.ma);

  irunit_t* u = unit(&c, n);

  // end
  map_dispose(&c.vars, c.ma);
end2:
  map_dispose(&c.funm, c.ma);
end1:
  for (usize i = 0; i < countof(c.tmpbuf); i++)
    buf_dispose(&c.tmpbuf[i]);
  fstatearray_dispose(&c.fstack, c.ma);

  dispose_maparray(c.ma, &c.defvars);
  dispose_maparray(c.ma, &c.pendingphis);
  dispose_maparray(c.ma, &c.freemaps);

  *result = u == &bad_irunit ? NULL : u;
  return c.err;
}


static bool dump_irunit(const irunit_t* u, memalloc_t ma) {
  buf_t buf = buf_make(ma);
  if (!irfmt(&buf, u)) {
    fprintf(stderr, "(irfmt failed)\n");
    return false;
  }
  fwrite(buf.chars, buf.len, 1, stderr);
  fputc('\n', stderr);
  buf_dispose(&buf);
  return true;
}


err_t analyze2(compiler_t* compiler, memalloc_t ir_ma, unit_t* unit) {
  irunit_t* u;
  err_t err = ircons(compiler, ir_ma, unit, &u);
  assertnotnull(u);
  dump_irunit(u, compiler->ma);
  return err;
}
