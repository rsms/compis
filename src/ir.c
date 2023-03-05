// SPDX-License-Identifier: Apache-2.0
#include "colib.h"
#include "bits.h"
#include "compiler.h"
#include <stdlib.h> // for debug_graphviz hack


typedef array_type(map_t) maparray_t;
DEF_ARRAY_TYPE_API(map_t, maparray)


typedef struct {
  compiler_t* compiler;
  memalloc_t  ma;          // compiler->ma
  memalloc_t  ir_ma;       // allocator for ir data
  irunit_t*   unit;        // current unit
  irfun_t*    f;           // current function
  irblock_t*  b;           // current block
  err_t       err;         // result of build process
  u32         condnest;    // >0 when inside conditional ("if", "for", etc)
  ptrarray_t  funqueue;    // [fun_t*] queue of functions awaiting build
  map_t       funm;        // {fun_t* => irfun_t*} for breaking cycles
  map_t       vars;        // {sym_t => irval_t*} (moved to defvars by end_block)
  maparray_t  defvars;     // {[block_id] => map_t}
  maparray_t  pendingphis; // {[block_id] => map_t}
  maparray_t  freemaps;    // free map_t's (for defvars and pendingphis)
  bitset_t*   deadset;
  ptrarray_t  dropstack;   // droparray_t*[]

  struct {
    ptrarray_t entries;
    u32        base;   // current scope's base index
  } owners;

  #if DEBUG
    int traceindent;
  #endif
} ircons_t;


#define trace(fmt, va...) \
  _trace(opt_trace_ir, 1, "IR", "%*s" fmt, c->traceindent*2, "", ##va)


static irval_t   bad_irval = { 0 };
static irblock_t bad_irblock = { 0 };
static funtype_t bad_astfuntype = { .kind = TYPE_FUN };
static fun_t     bad_astfun = { .kind = EXPR_FUN, .type = (type_t*)&bad_astfuntype };
static irfun_t   bad_irfun = { .ast = &bad_astfun };
static irunit_t  bad_irunit = { 0 };


static void seterr(ircons_t* c, err_t err) {
  if (!c->err)
    dlog("error set to: %d \"%s\"", err, err_str(err));
  c->err += (err * !c->err); // only set if c->err==0 (first error "wins")
}


#define ID_CHAR(v_or_b) _Generic((v_or_b), \
  irval_t*:   'v', \
  irblock_t*: 'b' )


ATTR_FORMAT(printf, 3, 4)
static const char* fmttmp(ircons_t* c, u32 bufindex, const char* fmt, ...) {
  buf_t* buf = tmpbuf_get(bufindex);
  va_list ap;
  va_start(ap, fmt);
  buf_vprintf(buf, fmt, ap);
  va_end(ap);
  return buf->chars;
}


UNUSED static const char* fmtnode(u32 bufindex, const void* nullable n) {
  buf_t* buf = tmpbuf_get(bufindex);
  safecheckexpr( node_fmt(buf, n, /*depth*/0), ErrOk);
  return buf->chars;
}


#ifdef DEBUG
  // static void trace_node(ircons_t* c, const char* msg, const node_t* n)
  #define trace_node(msg, n) \
    trace("%s%-14s: %s", (msg), nodekind_name((n)->kind), fmtnode(0, (n)))
  static void _traceindent_decr(void* cp) { (*(ircons_t**)cp)->traceindent--; }

  #define TRACE_SCOPE() \
    c->traceindent++; \
    ircons_t* c2 __attribute__((__cleanup__(_traceindent_decr),__unused__)) = c

  #define TRACE_NODE(prefix, n) \
    trace_node(prefix, (node_t*)n); \
    TRACE_SCOPE();
#else
  #define TRACE_NODE(prefix, n) ((void)0)
  #define TRACE_SCOPE()         ((void)0)
#endif


static void debug_graphviz(const compiler_t* c, const irunit_t* u) {
  buf_t buf = buf_make(c->ma);

  // generate graphviz "dot" text data
  if UNLIKELY(!irfmt_dot(c, &buf, u)) {
    fprintf(stderr, "(irfmt_dot failed)\n");
    buf_dispose(&buf);
    return;
  }
  // dlog("dot:\n———————————\n%s\n———————————", buf.chars);

  // write .dot file
  log("irdot ir.dot");
  err_t err = writefile("ir.dot", 0664, buf_slice(buf));
  if (err) {
    fprintf(stderr, "failed to write file ir.dot: %s", err_str(err));
    goto end;
  }

  #if DEBUG
    // invoke "dot" program
    buf_clear(&buf);
    buf_print(&buf, "dot -Tpng -oir.png ir.dot &");
    buf_nullterm(&buf);
    dlog("running '%s' ...", buf.chars);
    system(buf.chars);
  #endif

end:
  buf_dispose(&buf);
}


static bool dump_irunit(const compiler_t* c, const irunit_t* u) {
  buf_t buf = buf_make(c->ma);
  if (!irfmt(c, &buf, u)) {
    fprintf(stderr, "(irfmt failed)\n");
    buf_dispose(&buf);
    return false;
  }
  fwrite(buf.chars, buf.len, 1, stderr);
  fputc('\n', stderr);
  buf_dispose(&buf);
  return true;
}


// static bool dump_irfun(const irfun_t* f, memalloc_t ma, const locmap_t* lm) {
//   buf_t buf = buf_make(ma);
//   if (!irfmt_fun(&buf, f, lm)) {
//     fprintf(stderr, "(irfmt_fun failed)\n");
//     buf_dispose(&buf);
//     return false;
//   }
//   fwrite(buf.chars, buf.len, 1, stderr);
//   fputc('\n', stderr);
//   buf_dispose(&buf);
//   return true;
// }


inline static locmap_t* locmap(ircons_t* c) {
  return &c->compiler->locmap;
}


// void out_of_mem(ircons_t* c)
#define out_of_mem(c) ( dlog("OUT OF MEMORY"), seterr(c, ErrNoMem) )


// const origin_t to_origin(ircons_t*, T origin)
// where T is one of: origin_t | loc_t | irval_t* | node_t* | expr_t*
#define to_origin(c, origin) ({ \
  __typeof__(origin)* __tmp = &origin; \
  const origin_t __origin = _Generic(__tmp, \
          origin_t*:  *(origin_t*)__tmp, \
    const origin_t*:  *(origin_t*)__tmp, \
          loc_t*:     origin_make(locmap(c), *(loc_t*)__tmp), \
    const loc_t*:     origin_make(locmap(c), *(loc_t*)__tmp), \
          irval_t**:  origin_make(locmap(c), (*(irval_t**)__tmp)->loc), \
    const irval_t**:  origin_make(locmap(c), (*(irval_t**)__tmp)->loc), \
          node_t**:   node_origin(locmap(c), *(node_t**)__tmp), \
    const node_t**:   node_origin(locmap(c), *(node_t**)__tmp), \
          expr_t**:   node_origin(locmap(c), *(node_t**)__tmp), \
    const expr_t**:   node_origin(locmap(c), *(node_t**)__tmp) \
  ); \
  __origin; \
})

// void diag(ircons_t*, T origin, diagkind_t diagkind, const char* fmt, ...)
// where T is one of: origin_t | loc_t | irval_t* | node_t* | expr_t*
#define diag(c, origin, diagkind, fmt, args...) \
  report_diag((c)->compiler, to_origin((c), (origin)), (diagkind), (fmt), ##args)

#define error(c, origin, fmt, args...)    diag(c, origin, DIAG_ERR, (fmt), ##args)
#define warning(c, origin, fmt, args...)  diag(c, origin, DIAG_WARN, (fmt), ##args)
#define help(c, origin, fmt, args...)     diag(c, origin, DIAG_HELP, (fmt), ##args)


// static type_t* basetype(type_t* t) {
//   // unwrap optional and ref
//   assertnotnull(t);
//   if (t->kind == TYPE_OPTIONAL)
//     t = assertnotnull(((opttype_t*)t)->elem);
//   if (t->kind == TYPE_REF)
//     t = assertnotnull(((reftype_t*)t)->elem);
//   return t;
// }


// static bool isliveowner(ircons_t* c, const irval_t* v) {
//   return type_isowner(v->type) && !bitset_has(c->deadset, v->id);
// }


// bitset_t* deadset_copy(ircons_t* c, const bitset_t* src)
#define deadset_copy(c, src_deadset) ({ \
  bitset_t* __bs = bitset_make((c)->ma, (src_deadset)->cap); \
  (__bs && bitset_copy(&__bs, (src_deadset), (c)->ma)) ? \
    __bs : \
    (out_of_mem(c), (src_deadset)); \
})


static bool deadset_has(const bitset_t* bs, u32 id) {
  return bs->cap > (usize)id && bitset_has(bs, (usize)id);
}


static void deadset_add(ircons_t* c, bitset_t** bsp, u32 id) {
  if UNLIKELY(!bitset_ensure_cap(bsp, c->ma, id+1))
    return out_of_mem(c);
  bitset_add(*bsp, id);
}


// static void deadset_del(bitset_t* bs, u32 id) {
//   if (bs->cap > (usize)id)
//     bitset_del(bs, id);
// }


static irval_t* nullable find_arg_parent(ircons_t* c, u32 argid) {
  // Searches the current function's value for an argument with id,
  // returns the latest value which has v{argid} as argument.
  // Only used for diagnostics so doesn't have to be fast.
  for (u32 bi = c->f->blocks.len; bi != 0;) {
    irblock_t* b = c->f->blocks.v[--bi];
    for (u32 vi = b->values.len; vi != 0;) {
      irval_t* v = b->values.v[--vi];
      for (u32 ai = v->argc; ai != 0;) {
        irval_t* arg = v->argv[--ai];
        if (arg->id == argid)
          return v;
      }
    }
  }
  return NULL;
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
  maparray_remove(a, block_id, 1);
}


static map_t* assign_block_map(ircons_t* c, maparray_t* a, u32 block_id) {
  static map_t last_resort_map = {0};
  if (a->len <= block_id) {
    // fill holes
    u32 nholes = (block_id - a->len) + 1;
    map_t* p = maparray_alloc(a, c->ma, nholes);
    if UNLIKELY(!p)
      return out_of_mem(c), &last_resort_map;
    memset(p, 0, sizeof(map_t) * nholes);
  }
  if (a->v[block_id].cap == 0) {
    if UNLIKELY(!alloc_map(c, &a->v[block_id]))
      return out_of_mem(c), &last_resort_map;
  }
  return &a->v[block_id];
}


#define commentf(c, obj, fmt, args...) _Generic((obj), \
  irval_t*:   val_comment, \
  irblock_t*: block_comment )( (c), (void*)(obj), fmttmp((c), 0, (fmt), ##args) )

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


static irval_t* mkval(ircons_t* c, op_t op, loc_t loc, type_t* type) {
  irval_t* v = mem_alloct(c->ir_ma, irval_t);
  if UNLIKELY(v == NULL)
    return out_of_mem(c), &bad_irval;

  type = (type_t*)canonical_primtype(c->compiler, type);

  v->id = c->f->vidgen++;
  v->op = op;
  v->loc = loc;
  v->type = type;
  return v;
}


static irval_t* pushval(ircons_t* c, irblock_t* b, op_t op, loc_t loc, type_t* type) {
  irval_t* v = mkval(c, op, loc, type);
  if UNLIKELY(!ptrarray_push(&b->values, c->ir_ma, v))
    out_of_mem(c);
  return v;
}


static irval_t* insertval(
  ircons_t* c, irblock_t* b, u32 at_index, op_t op, loc_t loc, type_t* type)
{
  irval_t* v = mkval(c, op, loc, type);
  irval_t** vp = (irval_t**)ptrarray_allocat(&b->values, c->ir_ma, at_index, 1);
  if UNLIKELY(!vp)
    return out_of_mem(c), v;
  *vp = v;
  return v;
}


#define push_TODO_val(c, b, type, fmt, args...) ( \
  dlog("TODO_val " fmt, ##args), \
  comment((c), pushval((c), (b), OP_NOOP, (loc_t){0}, (type)), "TODO") \
)


static void pusharg(irval_t* dst, irval_t* arg) {
  arg->nuse++;
  if UNLIKELY(dst->argc >= countof(dst->argv)) {
    dlog("TODO unbounded pusharg");
    // TODO: allow dynamic size & growth of v->argv
    // seterr(c, ErrCanceled);
    return;
  }
  dst->argv[dst->argc++] = arg;
}


static irblock_t* mkblock(ircons_t* c, irfun_t* f, irblockkind_t kind, loc_t loc) {
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
  assertf(b->preds[1] == NULL || b->preds[0],
    "has preds[1] (b%u) but no b->preds[0]", b->preds[1]->id);
  return (u32)!!b->preds[0] + (u32)!!b->preds[1];
}


inline static u32 nsuccs(const irblock_t* b) {
  static_assert(countof(b->succs) == 2, "countof(irblock_t.succs) changed");
  assertf(b->succs[1] == NULL || b->succs[0],
    "has succs[1] (b%u) but no b->succs[0]", b->succs[1]->id);
  return (u32)!!b->succs[0] + (u32)!!b->succs[1];
}


static irblock_t* irval_block(ircons_t* c, const irval_t* v) {
  for (u32 i = 0; i < c->f->blocks.len; i++) {
    irblock_t* b = c->f->blocks.v[i];
    for (u32 j = 0; j < b->values.len; j++) {
      if (v == b->values.v[j])
        return b;
    }
  }
  assertf(0, "v%u not found", v->id);
  return &bad_irblock;
}


static bool isrvalue(const void* expr) {
  assert(node_isexpr(expr));
  return ((const expr_t*)expr)->flags & NF_RVALUE;
}


#define assert_can_own(v_or_b) assertf(\
  _Generic((v_or_b), \
    irval_t*:   type_isptr(((irval_t*)(v_or_b))->type), \
    irblock_t*: true ), \
  "%c%u is not an owner type", ID_CHAR(v_or_b), (v_or_b)->id)



//—————————————————————————————————————————————————————————————————————————————————————


static irval_t* var_read_recursive(ircons_t*, irblock_t*, sym_t, type_t*, loc_t);


static void var_write_map(ircons_t* c, map_t* vars, sym_t name, irval_t* v) {
  irval_t** vp = (irval_t**)map_assign_ptr(vars, c->ma, name);
  if UNLIKELY(!vp)
    return out_of_mem(c);
  *vp = v;
}


static irval_t* var_read_map(
  ircons_t* c, irblock_t* b, map_t* vars, sym_t name, type_t* type, loc_t loc)
{
  irval_t** vp = (irval_t**)map_lookup_ptr(vars, name);
  if (vp)
    return assertnotnull(*vp);
  //trace("%s %s not found in b%u", __FUNCTION__, name, b->id);
  return var_read_recursive(c, b, name, type, loc);
}


static void var_write_inblock(ircons_t* c, irblock_t* b, sym_t name, irval_t* v) {
  //trace("%s %s = v%u (b%u)", __FUNCTION__, name, v->id, b->id);
  map_t* vars;
  if (b == c->b) {
    vars = &c->vars;
  } else {
    vars = assign_block_map(c, &c->defvars, b->id);
  }
  var_write_map(c, vars, name, v);
}


static irval_t* var_read_inblock(
  ircons_t* c, irblock_t* b, sym_t name, type_t* type, loc_t loc)
{
  // trace("%s %s in b%u", __FUNCTION__, name, b->id);
  assertf(b != c->b, "defvars not yet flushed; use var_read for current block");
  map_t* vars = assign_block_map(c, &c->defvars, b->id);
  return var_read_map(c, b, vars, name, type, loc);
}


static void var_write(ircons_t* c, sym_t name, irval_t* v) {
  trace("%s %s = v%u", __FUNCTION__, name, v->id);
  var_write_map(c, &c->vars, name, v);
}


static irval_t* var_read(ircons_t* c, sym_t name, type_t* type, loc_t loc) {
  trace("%s %s", __FUNCTION__, name);
  return var_read_map(c, c->b, &c->vars, name, type, loc);
}


static void add_pending_phi(ircons_t* c, irblock_t* b, irval_t* phi, sym_t name) {
  // tracks pending, incomplete phis that are completed by seal_block for
  // blocks that are sealed after they have started. This happens when preds
  // are not known at the time a block starts, but is known and registered
  // before the block ends.

  //trace("%s in b%u for %s", __FUNCTION__, b->id, name);

  map_t* phimap = assign_block_map(c, &c->pendingphis, b->id);
  void** vp = map_assign_ptr(phimap, c->ma, name);
  if UNLIKELY(!vp)
    return out_of_mem(c);
  assertf(*vp == NULL, "duplicate phi for %s", name);
  // if (*vp)dlog("——UNEXPECTED—— duplicate phi for %s: v%u", name, ((irval_t*)*vp)->id);
  phi->aux.ptr = b;
  *vp = phi;
}


static irval_t* var_read_recursive(
  ircons_t* c, irblock_t* b, sym_t name, type_t* type, loc_t loc)
{
  //trace("%s %s in b%u", __FUNCTION__, name, b->id);

  irval_t* v;

  if ((b->flags & IR_FL_SEALED) == 0) {
    // incomplete CFG
    //trace("  block b%u not yet sealed", b->id);
    v = pushval(c, b, OP_PHI, loc, type);
    comment(c, v, name);
    add_pending_phi(c, b, v, name);
  } else if (npreds(b) == 1) {
    //trace("  read in single predecessor b%u", b->preds[0]->id);
    // optimize the common case of single predecessor; no phi needed
    v = var_read_inblock(c, b->preds[0], name, type, loc);
  } else if (npreds(b) == 0) {
    // outside of function
    //trace("  outside of function (no predecessors)");
    // ... read_global(c, ...)
    v = push_TODO_val(c, b, type, "gvn");
  } else {
    // multiple predecessors
    //trace("  read in predecessors b%u, b%u", b->preds[0]->id, b->preds[1]->id);
    irval_t* v0 = var_read_inblock(c, b->preds[0], name, type, loc);
    irval_t* v1 = var_read_inblock(c, b->preds[1], name, type, loc);
    if (v0->id == v1->id) {
      var_write_inblock(c, b, name, v0);
      return v0;
    }
    v = pushval(c, b, OP_PHI, loc, type);
    comment(c, v, name);
    var_write_inblock(c, b, name, v);
    assert(npreds(b) == 2);
    pusharg(v, v0);
    pusharg(v, v1);
    return v;
  }

  var_write_inblock(c, b, name, v);
  return v;
}


//—————————————————————————————————————————————————————————————————————————————————————


static void set_control(ircons_t* c, irblock_t* b, irval_t* nullable v) {
  if (v)
    v->nuse++;
  if (b->control)
    b->control->nuse--;
  b->control = v;
}


static void seal_block(ircons_t* c, irblock_t* b) {
  // sets IR_FL_SEALED, indicating that no further predecessors will be added
  //trace("%s b%u", __FUNCTION__, b->id);
  assert((b->flags & IR_FL_SEALED) == 0);
  b->flags |= IR_FL_SEALED;

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

  #ifdef DEBUG
    if (opt_trace_ir) {
      trace("stash %u var%s accessed by b%u", c->vars.len, c->vars.len==1?"":"s", b->id);
      for (const mapent_t* e = map_it(&c->vars); map_itnext(&c->vars, &e); ) {
        irval_t* v = e->value;
        trace("  - %s %s = v%u", (const char*)e->key, fmtnode(0, v->type), v->id);
      }
    }
  #endif

  // save vars
  map_t* vars = assign_block_map(c, &c->defvars, b->id);
  //assertf(vars->len == 0, "block was reopened and variables modified");
  if (vars->len == 0) {
    // swap c.vars with defvars
    map_t newmap = *vars;
    *vars = c->vars;
    c->vars = newmap;
  } else {
    // merge c.vars into existing defvars
    if UNLIKELY(!map_reserve(vars, c->ma, c->vars.len))
      return out_of_mem(c);
    for (const mapent_t* e = map_it(&c->vars); map_itnext(&c->vars, &e); ) {
      void** vp = map_assign_ptr(vars, c->ma, e->key);
      assertnotnull(vp); // earlier map_reserve should prevent this from happening
      *vp = e->value;
    }
    map_clear(&c->vars);
  }

}


static irblock_t* end_block(ircons_t* c) {
  // transfer live locals and seals c->b if needed
  trace("%s b%u", __FUNCTION__, c->b->id);

  irblock_t* b = c->b;
  c->b = &bad_irblock;
  assertf(b != &bad_irblock, "unbalanced start_block/end_block");

  stash_block_vars(c, b);

  if (!(b->flags & IR_FL_SEALED)) {
    seal_block(c, b);
  } else {
    assertf(get_block_map(&c->pendingphis, b->id) == NULL,
      "sealed block with pending PHIs");
  }

  // // dump state of vars
  // #ifdef DEBUG
  // if (opt_trace_ir) {
  //   trace("defvars");
  //   for (u32 bi = 0; bi < c->defvars.len; bi++) {
  //     map_t* vars = &c->defvars.v[bi];
  //     if (vars->len == 0)
  //       continue;
  //     trace("  b%u", bi);
  //     for (const mapent_t* e = map_it(vars); map_itnext(vars, &e); ) {
  //       sym_t name = e->key;
  //       irval_t* v = e->value;
  //       trace("    %s %s = v%u", name, fmtnode(0, v->type), v->id);
  //     }
  //   }
  // }
  // #endif

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

  // remove b from current function's blocks
  u32 i = blocks->len;
  while (i) {
    if (blocks->v[--i] == b)
      break;
  }
  assertf(i != 0, "b%u not in current function", b->id);
  ptrarray_remove(blocks, i, 1);
}


static irblock_t* entry_block(irfun_t* f) {
  assert(f->blocks.len > 0);
  return f->blocks.v[0];
}


//—————————————————————————————————————————————————————————————————————————————————————


static irval_t* intconst(ircons_t* c, type_t* t, u64 value, loc_t loc);


static void create_liveness_var(ircons_t* c, irval_t* v) {
  assert(v->var.live == NULL);

  // create initial (always true) liveness var in the block that defines v
  char tmp[32];
  sym_t name = sym_snprintf(tmp, sizeof(tmp), ".v%u_live", v->id);
  v->var.live = name;

  // initially dead or alive?
  bool islive = !deadset_has(c->deadset, v->id);
  irval_t* islivev = intconst(c, type_bool, islive, (loc_t){0});

  irblock_t* b = irval_block(c, v);
  var_write_inblock(c, b, name, islivev);
}


static void write_liveness_var(ircons_t* c, irval_t* owner, bool islive) {
  if (owner->var.live == NULL)
    create_liveness_var(c, owner);
  irval_t* islivev = intconst(c, type_bool, (u64)islive, (loc_t){0});
  var_write_inblock(c, c->b, owner->var.live, islivev);
}


static void owners_enter_scope(ircons_t* c, droparray_t* drops) {
  trace("\e[1;32m%s\e[0m", __FUNCTION__);
  if UNLIKELY(!ptrarray_push(&c->owners.entries, c->ma, (void*)(uintptr)c->owners.base))
    out_of_mem(c);
  c->owners.base = c->owners.entries.len - 1;

  if UNLIKELY(!ptrarray_push(&c->dropstack, c->ma, drops))
    out_of_mem(c);
}

static void owners_leave_scope(ircons_t* c) {
  trace("\e[1;32m%s\e[0m", __FUNCTION__);
  c->owners.entries.len = c->owners.base;
  c->owners.base = (u32)(uintptr)c->owners.entries.v[c->owners.base];

  ptrarray_pop(&c->dropstack);
}


static void owners_add(ircons_t* c, irval_t* v) {
  trace("\e[1;32m%s\e[0m" " v%u", __FUNCTION__, v->id);
  assert(type_isowner(v->type));
  if UNLIKELY(!ptrarray_push(&c->owners.entries, c->ma, v))
    out_of_mem(c);
}


static void owners_del_at(ircons_t* c, u32 index) {
  trace("\e[1;32m%s\e[0m" " [%u]", __FUNCTION__, index);
  assert(c->owners.entries.len > index);
  assertf(c->owners.base < index, "index(%u) is below base(%u)", index, c->owners.base);
  ptrarray_remove(&c->owners.entries, index, 1);
}


static u32 owners_indexof(ircons_t* c, irval_t* v, u32 depth) {
  u32 i = c->owners.entries.len;
  u32 base = c->owners.base;
  while (i > 1) {
    if (--i == base) {
      if (depth == 0)
        break;
      depth--;
      base = (u32)(uintptr)c->owners.entries.v[i];
    } else if (c->owners.entries.v[i] == v) {
      return i;
    }
  }
  return U32_MAX;
}


// static const char* fmtdeadset(ircons_t* c, u32 bufidx, const bitset_t* bs) {
//   buf_t* buf = &c->tmpbuf[bufidx];
//   buf_clear(buf);
//   buf_reserve(buf, bs->cap); // approximation
//   buf_push(buf, '{');
//   for (usize i = 0; i < bs->cap; i++) {
//     if (bitset_has(bs, i)) {
//       if (buf->len > 1)
//         buf_push(buf, ' ');
//       buf_printf(buf, "v%zu", i);
//     }
//   }
//   buf_push(buf, '}');
//   buf_nullterm(buf);
//   return buf->chars;
// }


static bool zeroinit_owner_needs_drop(ircons_t* c, type_t* t) {
  // return true for any owning type which for its zeroinit needs drop()
  return false;
}


static void backpropagate_drop_to_ast(ircons_t* c, irval_t* v, irval_t* dropv) {
  assertf(c->dropstack.len, "drop outside owners scope");
  droparray_t* drops = c->dropstack.v[c->dropstack.len - 1];

  sym_t name = v->var.dst ? v->var.dst : v->var.src;
  if (!name)
    name = dropv->var.dst ? dropv->var.dst : dropv->var.src;

  assertf(name != NULL, "%s of v%u without var name", __FUNCTION__, v->id);
  // if this is triggered, there might be a bug in assign_local

  // TODO FIXME ast_ma instead of ir_ma:
  drop_t* d = droparray_alloc(drops, c->ir_ma, 1);
  if UNLIKELY(!d)
    return out_of_mem(c);
  d->name = name;
  d->type = v->type;
}


static void drop(ircons_t* c, irval_t* v, loc_t loc) {
  irval_t* dropv;
  if (v->op == OP_MOVE && v->nuse == 0 && irval_block(c, v) == c->b) {
    // simplify MOVE;DROP in same block into DROP, e.g.
    //   v2 *int = MOVE v1
    //   DROP v2
    // becomes
    //   DROP v1
    // This makes certain optimizations easier, like when there are two "if" branches
    // which both end up dropping the same value. E.g.
    //   fun consume_and_log_in_debug_builds(bool cond, x *int) void {
    //     if cond {
    //       debug_log(x)
    //     }
    //   }
    // becomes (in non-debug mode)
    //   b0:
    //     v0 bool = ARG 0
    //     v1 *int = ARG 1
    //     switch v0 -> b1 b2
    //   b1: // b0.then
    //     v2 *int = MOVE v1
    //     DROP v2
    //   goto -> b3
    //   b2: // b0.implicit_else
    //     DROP v1
    //   goto -> b3
    //   b3: // b0.cont
    //   ret
    // with this simplification it instead becomes
    //   b0:
    //     v0 bool = ARG 0
    //     v1 *int = ARG 1
    //     switch v0 -> b1 b2
    //   b1: // b0.then
    //     DROP v1
    //   goto -> b3
    //   b2: // b0.implicit_else
    //     DROP v1
    //   goto -> b3
    //   b3: // b0.cont
    //   ret
    // which can trivially be optimized later on into
    //   b0:
    //     v0 bool = ARG 0  // unused and can be removed, too
    //     v1 *int = ARG 1
    //     DROP v1
    //   ret
    //
    v->op = OP_DROP;
    v->type = type_void;
    // note: arg 0 is already the value to drop
    v->var.src = v->var.dst;

    // Since declaration order matters (for drops), move the converted value to
    // the end of the current block to make sure this "MOVE -> DROP" optimization
    // has the same semantics as the non-optimal path ("else" branch below.)
    if (c->b->values.v[c->b->values.len-1] != v) {
      u32 i = ptrarray_rindexof(&c->b->values, v);
      assert(i != U32_MAX);
      ptrarray_move_to_end(&c->b->values, i);
    }
    dropv = v;
    v = v->argv[0];
  } else {
    dropv = pushval(c, c->b, OP_DROP, loc, type_void);
    pusharg(dropv, v);
    dropv->var.src = v->var.dst;
    if (v->var.dst)
      comment(c, dropv, v->var.dst);
  }
  trace("\e[1;33m" "drop v%u in b%u" "\e[0m", v->id, c->b->id);
  backpropagate_drop_to_ast(c, v, dropv);
}


static void conditional_drop(ircons_t* c, irval_t* control, irval_t* owner) {
  // creates "if (!.vN_live) { drop(vN) }"
  irblock_t* ifb = end_block(c);

  irblock_t* deadb = mkblock(c, c->f, IR_BLOCK_GOTO, (loc_t){0});
  irblock_t* contb = mkblock(c, c->f, IR_BLOCK_GOTO, (loc_t){0});

  set_control(c, contb, ifb->control);
  contb->kind = ifb->kind;
  memcpy(contb->succs, ifb->succs, nsuccs(ifb)*sizeof(*ifb->succs));

  ifb->kind = IR_BLOCK_SWITCH;
  set_control(c, ifb, control);

  ifb->succs[0] = contb; ifb->succs[1] = deadb;   // if -> cont, dead
  deadb->succs[0] = contb;                        // dead -> cont
  deadb->preds[0] = ifb;                          // dead <- if
  contb->preds[0] = ifb; contb->preds[1] = deadb; // cont <- if, dead
  commentf(c, deadb, "b%u.then", ifb->id);
  commentf(c, contb, "b%u.cont", ifb->id);

  start_block(c, deadb);
  seal_block(c, deadb);

  drop(c, owner, (loc_t){0});

  end_block(c);

  start_block(c, contb);
  seal_block(c, contb);
}


static void owners_unwind_one(ircons_t* c, const bitset_t* deadset, irval_t* v) {
  if (!deadset_has(deadset, v->id)) {
    // v definitely owns its value at the exit of its owning scope -- drop it
    trace("  v%u is live; drop right here in b%u", v->id, c->b->id);
    drop(c, v, (loc_t){0});
    return;
  }
  if (v->var.live) {
    // v may be owning its value, maybe not
    irval_t* liveness_var = var_read(c, v->var.live, type_bool, (loc_t){0});
    trace("  %s = v%u %s", v->var.live, liveness_var->id, op_name(liveness_var->op));
    if (liveness_var->op == OP_PHI) {
      trace("  v%u's ownership is runtime conditional", v->id);
      // ownership depends on what path the code takes; i.e. determined at runtime.
      // generate "if (!.vN_live) { drop(vN) }"
      conditional_drop(c, liveness_var, v);
      return;
    } else {
      // dlog("transitive liveness variable (%s = %s)",
      //   v->var.live, liveness_var->aux.i64val ? "true" : "false");
      assert(liveness_var->op == OP_ICONST);
      assert(liveness_var->aux.i64val == 0); // maybe legit. needs testing
      // ^ if hit, .vN_live==true -- revisit logic.
    }
  }
  // transitive liveness variable. i.e. a boolean constant like ".v0_live=false"
  trace("  v%u lost ownership", v->id);
}


static void owners_unwind_all(ircons_t* c) {
  trace("%s b%u (%u owners in scope)", __FUNCTION__, c->b->id, c->owners.entries.len);

  u32 i = c->owners.entries.len;
  u32 base = c->owners.base;
  while (i > 1) {
    if (--i == base) {
      base = (u32)(uintptr)c->owners.entries.v[i];
    } else {
      irval_t* v = c->owners.entries.v[i];
      owners_unwind_one(c, c->deadset, v);
    }
  }

  // empty current scope to prevent owners_unwind_scope from doing duplicate work
  c->owners.entries.len = c->owners.base;
}


static void owners_unwind_scope(ircons_t* c, bitset_t* entry_deadset) {
  // stop now if this scope has no owners (or: might have been unwound already)
  if (c->owners.entries.len == 0)
    return;

  assertf(c->b != &bad_irblock, "no current block");
  trace("%s ... b%u", __FUNCTION__, c->b->id);

  bitset_t* deadset = c->deadset;

  if (entry_deadset != deadset) {
    // xor computes the set difference between "dead before" and "dead after",
    // effectively "what values were killed in the scope"
    deadset = deadset_copy(c, c->deadset);
    if UNLIKELY(!bitset_merge_xor(&deadset, entry_deadset, c->ma))
      out_of_mem(c);
  }

  // iterate over owners defined in the current scope (parent of scope that closed)
  for (u32 i = c->owners.entries.len; --i > c->owners.base;) {
    irval_t* v = c->owners.entries.v[i];
    owners_unwind_one(c, deadset, v);
  }

  if (deadset != c->deadset)
    bitset_dispose(deadset, c->ma);
}


static u32 owners_find_lost(
  ircons_t* c, const bitset_t* entry_deadset, const bitset_t* exit_deadset)
{
  // returns index+1 of first (top of stack) drops.entries entry which
  // lost ownership since entry_deadset. (returns 0 if none found.)
  u32 i = c->owners.entries.len;
  u32 base = c->owners.base;
  while (i > 1) {
    if (--i == base) {
      base = (u32)(uintptr)c->owners.entries.v[i];
    } else {
      irval_t* v = c->owners.entries.v[i];
      if (!bitset_has(entry_deadset, v->id) && bitset_has(exit_deadset, v->id))
        return i + 1;
    }
  }
  return 0;
}


static void owners_drop_lost(
  ircons_t*       c,
  const bitset_t* entry_deadset,
  const bitset_t* exit_deadset,
  loc_t           loc,
  const char*     trace_msg
  )
{
  // drops values which lost ownership since entry_deadset. loc is used for DROPs.
  u32 i = c->owners.entries.len;
  u32 base = c->owners.base;
  while (i > 1) {
    i--;
    if (i == base) {
      base = (u32)(uintptr)c->owners.entries.v[i];
      continue;
    }
    irval_t* v = c->owners.entries.v[i];
    if (!bitset_has(entry_deadset, v->id) && bitset_has(exit_deadset, v->id)) {
      trace("  v%u lost ownership%s", v->id, trace_msg);
      assert(entry_deadset->cap > v->id);
      drop(c, v, loc);
      if (base == c->owners.base) {
        // belongs to the current scope; we can simply forget about this owner
        owners_del_at(c, i);
      } else {
        // belongs to a parent scope; update its liveness var
        write_liveness_var(c, v, false);
      }
    }
  }
}

static void move_owner(
  ircons_t* c,
  irval_t*          old_owner,
  irval_t* nullable new_owner,
  irval_t* nullable replace_owner)
{
  if (new_owner) {
    if (replace_owner) {
      trace("\e[1;33m" "move owner: v%u -> v%u, replacing v%u" "\e[0m",
        old_owner->id, new_owner->id, replace_owner->id);
      assert(type_isowner(replace_owner->type));
      u32 owners_index = owners_indexof(c, replace_owner, U32_MAX);
      if (owners_index != U32_MAX) {
        assertf(owners_index != U32_MAX, "owner v%u not found", replace_owner->id);
        c->owners.entries.v[owners_index] = new_owner;
        deadset_add(c, &c->deadset, replace_owner->id);
      }
    } else {
      trace("\e[1;33m" "move owner: v%u -> v%u" "\e[0m", old_owner->id, new_owner->id);
      owners_add(c, new_owner);
    }
    assertf(!deadset_has(c->deadset, new_owner->id), "v%u in deadset", new_owner->id);
    // deadset_del(c->deadset, new_owner->id);
  } else {
    trace("\e[1;33m" "move owner: v%u -> outside" "\e[0m", old_owner->id);
    assertf(replace_owner == NULL, "replace_owner without new_owner");
  }

  // mark old_owner as dead, no longer having ownership over its value
  deadset_add(c, &c->deadset, old_owner->id);

  // when on a conditional path, e.g. from "if", track liveness vars
  if (c->condnest) {
    // mark old_owner as no longer live by setting its liveness var to false
    write_liveness_var(c, old_owner, false);
    if (new_owner)
      write_liveness_var(c, new_owner, true);
  }
}


static void move_owner_outside(ircons_t* c, irval_t* old_owner) {
  move_owner(c, old_owner, NULL, NULL);
}


static irval_t* move(
  ircons_t* c, irval_t* rvalue, loc_t loc, irval_t* nullable replace_owner)
{
  if (rvalue->op == OP_PHI) {
    // rvalue is a PHI which means it joins two already-existing moves together
    return rvalue;
  }

  // The following is DISABLED because it introduces invalid state when an owning
  // var is defined with an initializer defined in the same scope, e.g.
  //   fun (x *int) *int {
  //     var a *int = x // transfer ownership of x's value to a
  //     var b = a      // transfer ownership of a's value to b
  //     return a       // BUG! a is believed to be live since a refers to value of x
  //   }
  // // If rvalue is a same-scope owner; additional move is redundant.
  // u32 i = owners_indexof(c, rvalue, 0); // in current scope?
  // if (i != U32_MAX) {
  //   // However, this optimization may introduce variance of drop order, and
  //   // drop order is important since an owner defined later may reference an owner
  //   // defined earlier. E.g.
  //   //   var a = create_thing()
  //   //   var b = create_thing()
  //   //   b.other_thing = &a     <— a must outlive b
  //   //   drop(b)
  //   //   drop(a)
  //   // So we must update the order of the owners scope by moving the owner on top.
  //   ptrarray_move_to_end(&c->owners.entries, i);
  //   return rvalue;
  // }

  irval_t* v = pushval(c, c->b, OP_MOVE, loc, rvalue->type);
  pusharg(v, rvalue);
  move_owner(c, rvalue, v, replace_owner);
  return v;
}


static irval_t* reference(ircons_t* c, irval_t* rvalue, loc_t loc) {
  op_t op = ((reftype_t*)rvalue->type)->kind == TYPE_MUTREF ? OP_BORROW_MUT : OP_BORROW;
  irval_t* v = pushval(c, c->b, op, loc, rvalue->type);
  pusharg(v, rvalue);
  return v;
}


static irval_t* move_or_copy(
  ircons_t* c, irval_t* rvalue, loc_t loc, irval_t* nullable replace_owner)
{
  irval_t* v = rvalue;
  if (type_isowner(rvalue->type)) {
    v = move(c, rvalue, loc, replace_owner);
  } else if (type_isref(rvalue->type)) {
    v = reference(c, rvalue, loc);
  }
  v->var.src = rvalue->var.dst;
  return v;
}


static type_t* unwind_aliastypes(type_t* t) {
  while (t->kind == TYPE_ALIAS)
    t = assertnotnull(((aliastype_t*)t)->elem);
  return t;
}


//—————————————————————————————————————————————————————————————————————————————————————


static irval_t* expr(ircons_t* c, void* expr_node);
static irval_t* load_expr(ircons_t* c, expr_t* n);
static irval_t* load_rvalue(ircons_t* c, expr_t* origin, expr_t* n);


static irval_t* intconst(ircons_t* c, type_t* t, u64 value, loc_t loc) {
  // "intern" constants
  // this is a really simple solution:
  // - all constants are placed at the beginning of the entry block
  //   - first int constants, then float constants
  // - linear scan for an existing equivalent constant
  // - fast for functions with few constants, which is the common case
  // - degrades for functions with many constants
  //   - could do binary search if we bookkeep ending index
  t = unwind_aliastypes(t);
  irblock_t* b0 = entry_block(c->f);
  u32 i = 0;
  for (; i < b0->values.len; i++) {
    irval_t* v = b0->values.v[i];
    if (v->op != OP_ICONST || v->aux.i64val > value)
      break;
    if (v->aux.i64val == value && v->type == t)
      return v;
  }
  irval_t* v = insertval(c, b0, i, OP_ICONST, loc, t);
  v->aux.i64val = value;
  return v;
}


static irval_t* idexpr(ircons_t* c, idexpr_t* n) {
  assertnotnull(n->ref);
  assertf(node_islocal(n->ref), "%s", nodekind_name(n->ref->kind));
  local_t* local = (local_t*)n->ref;
  return var_read(c, local->name, local->type, local->loc);
}


static irval_t* param(ircons_t* c, local_t* n) {
  return var_read(c, n->name, n->type, n->loc);
}


static irval_t* assign_local(ircons_t* c, local_t* dst, irval_t* v) {
  sym_t name = dst->name;
  if (name == sym__) {
    assertf(!type_isowner(dst->type), "owner without temporary name");
    return v;
  }
  v->var.dst = name;
  var_write(c, name, v);
  return v;
}


static irval_t* vardef(ircons_t* c, local_t* n) {
  irval_t* v;
  if (n->init) {
    irval_t* v1 = load_expr(c, n->init);
    v1->type = n->type; // needed in case dst is subtype of v, e.g. "dst ?T <= v T"
    v = move_or_copy(c, v1, n->loc, NULL);
    if (n->name != sym__) {
      if (v == v1 && v->comment && *v->comment) {
        commentf(c, v, "%s aka %s", v->comment, n->name);
      } else {
        comment(c, v, n->name);
      }
    }
  } else {
    v = pushval(c, c->b, OP_ZERO, n->loc, n->type);
    if (n->name != sym__)
      comment(c, v, n->name);
    // owning var without initializer is initially dead
    if (type_isowner(v->type)) {
      // must owners_add explicitly since we don't pass replace_owner to move_or_copy
      owners_add(c, v);
      if (!zeroinit_owner_needs_drop(c, v->type)) {
        // mark as dead since the type's zeroinit doesn't need drop (no side effects)
        deadset_add(c, &c->deadset, v->id);
        // create_liveness_var(c, v);
      }
    }
  }
  return assign_local(c, n, v);
}


static irval_t* assign(ircons_t* c, binop_t* n) {
  irval_t* v = load_expr(c, n->right);
  local_t* dst = NULL;

  expr_t* left = n->left;
  while (left->kind == EXPR_DEREF) {
    dlog("TODO assignment to deref");
    left = ((unaryop_t*)left)->expr;
  }

  switch (left->kind) {
    case EXPR_MEMBER: {
      member_t* m = (member_t*)left;
      assertnotnull(m->target);
      assert(m->target->kind == EXPR_FIELD);
      dst = (local_t*)m->target;
      break;
    }
    case EXPR_ID: {
      idexpr_t* id = (idexpr_t*)left;
      dst = (local_t*)id->ref;
      // note: dst may be null, i.e in case of "_ = expr", "_" has no ref.
      if (!dst)
        return v;
      break;
    }
    default:
      assertf(0, "unexpected %s", nodekind_name(left->kind));
      return v;
  }

  assert(node_islocal((node_t*)dst));
  sym_t varname = dst->name;
  v->type = dst->type; // needed in case dst is subtype of v, e.g. "dst ?T <= v T"

  irval_t* curr_owner = var_read(c, varname, v->type, (loc_t){0});
  v = move_or_copy(c, v, n->loc, curr_owner);

  comment(c, v, varname);

  return assign_local(c, dst, v);
}


static irval_t* ret(ircons_t* c, irval_t* nullable v, loc_t loc) {
  c->b->kind = IR_BLOCK_RET;
  if (v && type_isowner(v->type))
    move_owner_outside(c, v);
  set_control(c, c->b, v);
  owners_unwind_all(c);
  return v ? v : &bad_irval;
}


static irval_t* retexpr(ircons_t* c, retexpr_t* n) {
  irval_t* v = n->value ? load_expr(c, n->value) : NULL;
  return ret(c, v, n->loc);
}


static irval_t* member(ircons_t* c, member_t* n) {
  assertnotnull(n->target);
  irval_t* recv = load_expr(c, n->recv);

  // TODO FIXME: this is incomplete

  irval_t* v = pushval(c, c->b, OP_GEP, n->loc, n->type);
  pusharg(v, recv);

  assertnotnull(n->target);
  if (n->target->kind == EXPR_FIELD) {
    local_t* field = (local_t*)n->target;
    v->aux.i64val = field->offset;
  } else {
    dlog("TODO target %s", nodekind_name(n->target->kind));
  }

  return v;
}


static irval_t* typecons(ircons_t* c, typecons_t* n) {
  if (n->expr == NULL)
    return pushval(c, c->b, OP_ZERO, n->loc, n->type);
  irval_t* src = load_expr(c, n->expr);
  irval_t* v = pushval(c, c->b, OP_CAST, n->loc, n->type);
  pusharg(v, src);
  return v;
}


static irval_t* call(ircons_t* c, call_t* n) {
  if (n->recv->kind == EXPR_ID && node_istype(((idexpr_t*)n->recv)->ref)) {
    return push_TODO_val(c, c->b, n->type, "type call");
  }

  irval_t* recv = load_expr(c, n->recv);

  irval_t* v = pushval(c, c->b, OP_CALL, n->loc, n->type);
  pusharg(v, recv);

  for (u32 i = 0; i < n->args.len; i++) {
    expr_t* arg = n->args.v[i];
    irval_t* arg_v = load_expr(c, arg);
    if (type_isowner(arg_v->type))
      move_owner_outside(c, arg_v);
    // if (arg_v->op != OP_MOVE)
    //   arg_v = move_or_copy(c, arg_v, arg->loc, NULL);
    pusharg(v, arg_v);
  }

  if (type_isowner(v->type))
    owners_add(c, v);
  return v;
}


static irval_t* blockexpr0(ircons_t* c, block_t* n, bool isfunbody) {
  if (n->children.len == 0) {
    if (isrvalue(n))
      return pushval(c, c->b, OP_ZERO, n->loc, n->type);
    return &bad_irval;
  }
  u32 lastrval = (n->children.len-1) + (u32)!isrvalue(n);

  for (u32 i = 0; i < n->children.len; i++) {
    expr_t* cn = n->children.v[i];

    if (i == lastrval && cn->kind != EXPR_RETURN) {
      irval_t* v = load_expr(c, cn);
      // note: if cn constitutes implicit return from a function, isfunbody==true:
      // fun() will call ret() to generate a return; no need to move_owner() here.
      if (!isfunbody) {
        if (v->op != OP_MOVE)
          v = move_or_copy(c, v, cn->loc, NULL);
        // move to lvalue of block (NULL b/c unknown for now)
        if (type_isowner(v->type))
          move_owner_outside(c, v);
      }
      commentf(c, v, "b%u", c->b->id);
      return v;
    }

    expr(c, cn);
    if (cn->kind == EXPR_RETURN)
      break;
  }
  return &bad_irval;
}


static irval_t* blockexpr_noscope(ircons_t* c, block_t* n, bool isfunbody) {
  TRACE_NODE("expr ", n);
  return blockexpr0(c, n, isfunbody);
}


static irval_t* blockexpr(ircons_t* c, block_t* n) {
  #define CREATE_BB_FOR_BLOCK

  #ifdef CREATE_BB_FOR_BLOCK
    irblock_t* prevb = end_block(c);
    prevb->kind = IR_BLOCK_GOTO;

    irblock_t* b = mkblock(c, c->f, IR_BLOCK_GOTO, n->loc);
    irblock_t* contb = mkblock(c, c->f, IR_BLOCK_GOTO, n->loc);

    prevb->succs[0] = b;
    b->preds[0] = prevb;
    b->succs[0] = contb;
    contb->preds[0] = b;

    start_block(c, b);
    seal_block(c, b);
  #endif

  owners_enter_scope(c, &n->drops);

  irval_t* v = blockexpr0(c, n, /*isfunbody*/false);

  #ifdef CREATE_BB_FOR_BLOCK
    b = end_block(c);
    start_block(c, contb);
    seal_block(c, contb);
  #endif

  owners_unwind_scope(c, c->deadset);
  owners_leave_scope(c);

  return v;
}


static irval_t* bincond(ircons_t* c, expr_t* n) {
  // binary conditional is either a boolean or optional

  // TODO: "!x"

  irval_t* v = load_expr(c, n);
  if (v->type == type_bool)
    return v;

  assertf(v->type->kind == TYPE_OPTIONAL, "%s", nodekind_name(v->type->kind));

  irval_t* optcheck = pushval(c, c->b, OP_OCHECK, n->loc, type_bool);
  pusharg(optcheck, v);
  return optcheck;
}


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
  //
  irfun_t* f = c->f;
  c->condnest++;

  // generate control condition
  irval_t* control = bincond(c, n->cond);

  // end predecessor block (leading up to and including "if")
  irblock_t* ifb = end_block(c);
  ifb->kind = IR_BLOCK_SWITCH;
  set_control(c, ifb, control);

  // create blocks for then and else branches
  irblock_t* thenb = mkblock(c, f, IR_BLOCK_GOTO, n->thenb->loc);
  irblock_t* elseb = mkblock(c, f, IR_BLOCK_GOTO, n->elseb ? n->elseb->loc : n->loc);
  u32        elseb_index = f->blocks.len - 1; // used later for moving blocks
  ifb->succs[1] = thenb;
  ifb->succs[0] = elseb; // switch control -> [else, then]
  commentf(c, thenb, "b%u.then", ifb->id);

  // copy deadset as is before entering "then" branch, in case it returns
  bitset_t* entry_deadset = deadset_copy(c, c->deadset);

  // begin "then" branch
  trace("if \"then\" branch");
  thenb->preds[0] = ifb; // then <- if
  start_block(c, thenb);
  seal_block(c, thenb);
  owners_enter_scope(c, &n->thenb->drops);
  irval_t* thenv = blockexpr_noscope(c, n->thenb, /*isfunbody*/false);
  owners_unwind_scope(c, entry_deadset);
  owners_leave_scope(c);
  u32 thenb_nvars = c->vars.len; // save number of vars modified by the "then" branch

  // if "then" branch returns, undo deadset changes made by the "then" branch,
  // or there's an "else" branch which needs deadset state before "then" branch.
  bitset_t* then_entry_deadset;
  if (c->b->kind == IR_BLOCK_RET || n->elseb) {
    if (n->elseb) {
      // copy deadset as is before entering "else" branch
      then_entry_deadset = deadset_copy(c, c->deadset);
    }
    if UNLIKELY(!bitset_copy(&c->deadset, entry_deadset, c->ma))
      out_of_mem(c);
  }

  // end & seal "then" block
  thenb = end_block(c);

  irval_t* elsev;

  // begin "else" branch (if there is one)
  if (n->elseb) {
    trace("if \"else\" branch");

    // begin "else" block
    commentf(c, elseb, "b%u.else", ifb->id);
    elseb->preds[0] = ifb; // else <- if
    start_block(c, elseb);
    seal_block(c, elseb);
    owners_enter_scope(c, &n->elseb->drops);
    elsev = blockexpr_noscope(c, n->elseb, /*isfunbody*/false);
    owners_unwind_scope(c, entry_deadset);
    owners_leave_scope(c);

    // if "then" block returns, no "cont" block needed
    // e.g. "fun f() int { if true { 1 } else { return 2 }; 3 }"
    if (thenb->kind == IR_BLOCK_RET) {
      // discard_block(c, contb);
      bitset_dispose(entry_deadset, c->ma);
      bitset_dispose(then_entry_deadset, c->ma);
      c->condnest--;
      return elsev;
    }

    // generate drops in "else" branch for owners lost in "then" branch
    // note: must run in the "if"-parent scope, not in a branch's scope.
    //dlog("entry_deadset:\n  %s", fmtdeadset(c, 0, entry_deadset));
    //dlog("c->deadset:\n  %s", fmtdeadset(c, 0, c->deadset));
    owners_drop_lost(c, c->deadset, then_entry_deadset, n->loc, " in \"then\" branch");

    // end "else" block
    u32 elseb_nvars = c->vars.len; // save number of vars modified by the "else" block
    elseb = end_block(c);

    // if "else" block returns, undo deadset changes made by the "else" block
    if (elseb->kind == IR_BLOCK_RET) {
      trace("\"else\" block returns -- undo deadset changes from \"else\" block");
      if UNLIKELY(!bitset_copy(&c->deadset, then_entry_deadset, c->ma))
        out_of_mem(c);
    } else if (owners_find_lost(c, then_entry_deadset, c->deadset)) {
      // generate drops in "then" branch for owners lost in "else" branch.
      // note: must run in the "if"-parent scope, not in a branch's scope.
      start_block(c, thenb);
      owners_drop_lost(c, then_entry_deadset, c->deadset, n->loc, " in \"else\" branch");
      end_block(c);
    }

    // merge ownership losses that happened in the "then" branch into "after if"
    if UNLIKELY(!bitset_merge_union(&c->deadset, then_entry_deadset, c->ma))
      out_of_mem(c);
    bitset_dispose(then_entry_deadset, c->ma);

    // create continuation block (the block after the "if")
    irblock_t* contb = mkblock(c, f, IR_BLOCK_GOTO, n->loc);
    commentf(c, contb, "b%u.cont", ifb->id);

    // test if "then" or "else" blocks are empty without effects
    bool thenb_isnoop = !thenb->values.len && !thenb_nvars && thenb->preds[0] == ifb;
    bool elseb_isnoop = !elseb->values.len && !elseb_nvars && elseb->preds[0] == ifb;

    // wire up graph edges
    if UNLIKELY(thenb_isnoop && elseb_isnoop) {
      // none of the branches have any effect; cut both of them out
      trace("eliding \"then\" and \"else\" branches");
      // Note: we can't simply skip the continuation block because var_read_recursive
      // will look in predecessors to find a variable.
      // This happens after end_block which calls stash_block_vars which moves
      // c.vars ("vars of this block") to c.defvars ("vars of other blocs").
      // This is unavoidable since we must end_block(ifb) to build thenb and elseb.
      //
      // transform "if" block to simple "goto contb"
      ifb->kind = IR_BLOCK_GOTO;
      set_control(c, ifb, NULL);
      ifb->succs[0] = contb;
      ifb->succs[1] = NULL;
      contb->preds[0] = ifb;
      // discard unused blocks
      discard_block(c, elseb);
      discard_block(c, thenb);
      // prime for conditional later on
      thenv = elsev;
    } else if (thenb_isnoop) {
      // "then" branch has no effect; cut it out
      trace("eliding \"then\" branch");
      elseb->succs[0] = contb; // else —> cont
      ifb->succs[1] = contb;   // if true —> cont
      contb->preds[0] = elseb; // cont[0] <— else
      contb->preds[1] = ifb;   // cont[1] <— if
      discard_block(c, thenb); // trash thenb
      thenb = contb;
    } else if (elseb_isnoop) {
      // "else" branch has no effect; cut it out
      trace("eliding \"else\" branch");
      thenb->succs[0] = contb; // then —> cont
      ifb->succs[0] = contb;   // if false —> cont
      contb->preds[0] = ifb;   // cont[0] <— if
      contb->preds[1] = thenb; // cont[1] <— then
      discard_block(c, elseb); // trash elseb
      elseb = contb;
    } else {
      // both branches have effect
      elseb->succs[0] = contb; // else —> cont
      thenb->succs[0] = contb; // then —> cont
      if (thenb->kind == IR_BLOCK_RET) {
        contb->preds[0] = elseb; // cont[0] <— else
      } else if (elseb->kind == IR_BLOCK_RET) {
        contb->preds[0] = thenb; // cont[0] <— then
      } else {
        contb->preds[1] = thenb; // cont[1] <— then
        contb->preds[0] = elseb; // cont[0] <— else
      }
    }

    // begin continuation block
    start_block(c, contb);
    seal_block(c, contb);
  } else {
    // no "else" branch

    // check if "then" branch caused loss of ownership of outer values
    if (thenb->kind != IR_BLOCK_RET && owners_find_lost(c, entry_deadset, c->deadset)) {
      // begin "else" branch
      commentf(c, elseb, "b%u.implicit_else", ifb->id);
      elseb->preds[0] = ifb; // else <- if
      start_block(c, elseb);
      seal_block(c, elseb);

      // generate drops for values which lost ownership in the "then" branch
      owners_drop_lost(c, entry_deadset, c->deadset, n->loc, " in \"then\" branch");

      // end "else" branch
      elseb = end_block(c);

      // create continuation block (the block after the "if")
      irblock_t* contb = mkblock(c, f, IR_BLOCK_GOTO, n->loc);
      commentf(c, contb, "b%u.cont", ifb->id);

      // wire up graph edges
      elseb->succs[0] = contb; // else —> cont
      thenb->succs[0] = contb; // then —> cont
      contb->preds[1] = thenb; // cont[1] <— then
      contb->preds[0] = elseb; // cont[0] <— else

      // begin continuation block
      start_block(c, contb);
      seal_block(c, contb);
    } else {
      // convert elseb to "end" block
      commentf(c, elseb, "b%u.cont", ifb->id);
      thenb->succs[0] = elseb; // then -> else
      elseb->preds[0] = ifb;
      if (thenb->kind != IR_BLOCK_RET)
        elseb->preds[1] = thenb; // else <- if, then
      start_block(c, elseb);
      seal_block(c, elseb);

      // move cont block to end (in case blocks were created by "then" body)
      ptrarray_move(&f->blocks, f->blocks.len - 1, elseb_index, elseb_index + 1);
    }

    if (isrvalue(n)) {
      // zero in place of "else" block
      elsev = pushval(c, c->b, OP_ZERO, n->loc, thenv->type);
    } else {
      elsev = thenv;
    }
  }

  bitset_dispose(entry_deadset, c->ma);
  c->condnest--;

  // if the result of the "if" expression is not used, no PHI is needed
  if (!isrvalue(n) || thenv == elsev)
    return thenv;

  // make Phi, joining the two branches together
  assertf(c->b->preds[0], "phi in block without predecessors");
  irval_t* phi = pushval(c, c->b, OP_PHI, n->loc, thenv->type);
  pusharg(phi, thenv);
  pusharg(phi, elsev);
  comment(c, phi, "if");

  return phi;
}


static irval_t* binop(ircons_t* c, binop_t* n) {
  irval_t* left  = load_expr(c, n->left);
  irval_t* right = load_expr(c, n->right);
  assert(left->type && right->type);
  irval_t* v = pushval(c, c->b, n->op, n->loc, n->type);
  pusharg(v, left);
  pusharg(v, right);
  return v;
}


static irval_t* intlit(ircons_t* c, intlit_t* n) {
  return intconst(c, n->type, n->intval, n->loc);
}


static irval_t* strlit(ircons_t* c, strlit_t* n) {
  irval_t* v = pushval(c, c->b, OP_STR, n->loc, n->type);
  v->aux.bytes.bytes = n->bytes;
  v->aux.bytes.len = n->len;
  return v;
}


static irval_t* arraylit(ircons_t* c, arraylit_t* n) {
  irval_t* v = pushval(c, c->b, OP_ARRAY, n->loc, n->type);
  for (u32 i = 0; i < n->values.len; i++) {
    expr_t* cn = n->values.v[i];
    irval_t* vv = load_expr(c, cn);
    if (vv->op != OP_MOVE)
      vv = move_or_copy(c, vv, cn->loc, NULL);
    pusharg(v, vv);
  }
  comment(c, v, "arraylit");
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


// postorder_dfs provides a DFS postordering of blocks in f
static void postorder_dfs(ircons_t* c, irfun_t* f, irblock_t** order) {
  if (f->blocks.len == 0)
    return;

  // track which blocks we have visited to break cycles, using a bitset of block IDs
  bitset_t* visited = bitset_make(c->ma, f->bidgen);
  if UNLIKELY(!visited)
    return out_of_mem(c);

  // stack of block+next-child-index to visit
  // (bi_t.i is the number of successor edges of b that have already been visited)
  typedef struct { irblock_t* b; u32 i; } bi_t;
  bi_t* workstack = mem_alloctv(c->ma, bi_t, f->blocks.len);
  u32 worklen = 0;
  if UNLIKELY(!workstack) {
    bitset_dispose(visited, c->ma);
    return out_of_mem(c);
  }

  irblock_t* b0 = f->blocks.v[0];
  bitset_add(visited, b0->id);
  workstack[worklen++] = (bi_t){ b0, 0 };
  #if DEBUG
  u32 orderlen = 0;
  #endif

  while (worklen) {
    u32 tos = worklen - 1;
    irblock_t* b = assertnotnull(workstack[tos].b);
    u32 i = workstack[tos].i;

    if (i < nsuccs(b)) {
      workstack[tos].i++;
      irblock_t* bb = assertnotnull(b->succs[i]);
      if (!bitset_has(visited, bb->id)) {
        bitset_add(visited, bb->id);
        workstack[worklen++] = (bi_t){ bb, 0 };
      }
    } else {
      worklen--;
      *order++ = b;
      #if DEBUG
      orderlen++;
      #endif
    }
  }

  assertf(orderlen == f->blocks.len, "did not visit all blocks");

  bitset_dispose(visited, c->ma);
  mem_freetv(c->ma, workstack, f->blocks.len);
}


UNUSED static void check_borrowing(ircons_t* c, irfun_t* f) {
  // dump_irfun(f, c->ma);
  dlog("TODO %s", __FUNCTION__);

  // compute postorder of f's blocks
  irblock_t** postorder = mem_alloctv(c->ma, irblock_t*, f->blocks.len);
  if UNLIKELY(!postorder)
    return out_of_mem(c);
  postorder_dfs(c, f, postorder);
  dlog("postorder:");
  for (u32 i = 0; i < f->blocks.len; i++)
    dlog("  b%u", postorder[i]->id);
  mem_freetv(c->ma, postorder, f->blocks.len);
}


static bool addfun(ircons_t* c, fun_t* n, irfun_t** fp) {
  // make sure *fp is initialized no matter what happens
  *fp = &bad_irfun;

  // functions may refer to themselves, so we record "ongoing" functions in a map
  irfun_t** funmp = (irfun_t**)map_assign_ptr(&c->funm, c->ma, n);
  if UNLIKELY(!funmp)
    return out_of_mem(c), false;

  // stop short if function is already built or in progress of being built
  if (*funmp) {
    *fp = *funmp;
    return false;
  }

  // allocate irfun_t
  irfun_t* f = mem_alloct(c->ir_ma, irfun_t);
  if UNLIKELY(!f)
    return out_of_mem(c), &bad_irfun;
  *fp = f;
  *funmp = f;
  f->name = mem_strdup(c->ir_ma, slice_cstr(n->name), 0);
  f->ast = n;

  // add to current unit
  if UNLIKELY(!ptrarray_push(&c->unit->functions, c->ir_ma, f)) {
    out_of_mem(c);
    return false;
  }

  // just a declaration?
  if (n->body == NULL)
    return false;

  // handle function refs and nested function definitions
  if (c->f != &bad_irfun) {
    trace("funqueue push %s", fmtnode(0, n));
    if UNLIKELY(!ptrarray_push(&c->funqueue, c->ma, n))
      out_of_mem(c);
    return false;
  }

  return true;
}


static irfun_t* fun(ircons_t* c, fun_t* n, irfun_t* nullable f) {
  if (f == NULL && !addfun(c, n, &f))
    return f;

  c->f = f;
  c->condnest = 0;
  c->owners.entries.len = 0;
  c->owners.base = 0;
  bitset_clear(c->deadset);

  // allocate entry block
  irblock_t* entryb = mkblock(c, f, IR_BLOCK_GOTO, n->loc);
  start_block(c, entryb);
  seal_block(c, entryb); // entry block has no predecessors

  // enter function scope
  owners_enter_scope(c, &n->body->drops);

  funtype_t* ft = (funtype_t*)n->type;

  // define arguments
  for (u32 i = 0; i < ft->params.len; i++) {
    local_t* param = ft->params.v[i];
    if (param->name == sym__)
      continue;
    irval_t* v = pushval(c, c->b, OP_ARG, param->loc, param->type);
    v->aux.i32val = i;
    v->var.dst = param->name;
    comment(c, v, param->name);

    if (type_isowner(param->type))
      owners_add(c, v);

    var_write(c, param->name, v);
  }

  // check if function has implicit return value
  if (ft->result != type_void && n->body->children.len) {
    expr_t* lastexpr = n->body->children.v[n->body->children.len-1];
    if (lastexpr->kind != EXPR_RETURN)
      n->body->flags |= NF_RVALUE;
  }

  bitset_t* entry_deadset = deadset_copy(c, c->deadset);

  // build body
  irval_t* body = blockexpr_noscope(c, n->body, /*isfunbody*/true);

  // reset NF_RVALUE flag, in case we set it above
  n->body->flags &= ~NF_RVALUE;

  // handle implicit return
  // note: if the block ended with a "return" statement, b->kind is already BLOCK_RET
  if (c->b->kind != IR_BLOCK_RET)
    ret(c, body != &bad_irval ? body : NULL, n->body->loc);

  // leave function scope
  owners_unwind_scope(c, entry_deadset);
  owners_leave_scope(c);
  bitset_dispose(entry_deadset, c->ma);

  // end final block of the function
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

  //check_borrowing(c, f);

  c->f = &bad_irfun;
  return f;
}


static irval_t* funexpr(ircons_t* c, fun_t* n) {
  irfun_t* f = fun(c, n, NULL);
  irval_t* v = pushval(c, c->b, OP_FUN, n->loc, n->type);
  v->aux.ptr = f;
  if (f->name)
    comment(c, v, f->name);
  return v;
}


static irval_t* deref(ircons_t* c, expr_t* origin, unaryop_t* n) {
  irval_t* src = load_rvalue(c, origin, n->expr);
  irval_t* v = pushval(c, c->b, OP_DEREF, origin->loc, origin->type);
  pusharg(v, src);
  return v;
}


static irval_t* load_local(ircons_t* c, expr_t* origin, local_t* n) {
  irval_t* v = var_read(c, n->name, n->type, n->loc);
  if LIKELY(!type_isowner(n->type) || !bitset_has(c->deadset, v->id))
    return v;

  // owner without ownership of a value

  irval_t* parentv = find_arg_parent(c, v->id);

  if (!parentv && v->op == OP_ZERO) {
    error(c, origin, "use of uninitialized %s %s", nodekind_fmt(n->kind), n->name);
    if (loc_line(v->loc))
      help(c, v, "%s defined here", n->name);
    return v;
  }

  error(c, origin, "use of dead value %s", n->name);
  if (parentv && parentv->op == OP_MOVE && loc_line(parentv->loc))
    help(c, parentv, "%s moved here", n->name);

  return v;
}


static irval_t* load_rvalue(ircons_t* c, expr_t* origin, expr_t* n) {
  trace("\e[1;35m" "load %s %s" "\e[0m", nodekind_fmt(n->kind), fmtnode(0, n));
  TRACE_SCOPE();

  switch (n->kind) {
  case EXPR_ID:
    return load_rvalue(c, origin, asexpr(((idexpr_t*)n)->ref));

  case EXPR_FIELD:
  case EXPR_PARAM:
  case EXPR_LET:
  case EXPR_VAR:
    return load_local(c, origin, (local_t*)n);

  default:
    return expr(c, n);
  }
}


static irval_t* load_expr(ircons_t* c, expr_t* n) {
  if (n->kind == EXPR_ID) {
    node_t* rvalue = ((idexpr_t*)n)->ref;
    assert(node_isexpr(rvalue));
    return load_rvalue(c, n, (expr_t*)rvalue);
  }
  return expr(c, n);
}


static irval_t* expr(ircons_t* c, void* expr_node) {
  expr_t* n = expr_node;
  TRACE_NODE("expr ", n);

  switch ((enum nodekind)n->kind) {
  case EXPR_ASSIGN:    return assign(c, (binop_t*)n);
  case EXPR_BINOP:     return binop(c, (binop_t*)n);
  case EXPR_BLOCK:     return blockexpr(c, (block_t*)n);
  case EXPR_CALL:      return call(c, (call_t*)n);
  case EXPR_TYPECONS:  return typecons(c, (typecons_t*)n);
  case EXPR_DEREF:     return deref(c, n, (unaryop_t*)n);
  case EXPR_ID:        return idexpr(c, (idexpr_t*)n);
  case EXPR_FUN:       return funexpr(c, (fun_t*)n);
  case EXPR_IF:        return ifexpr(c, (ifexpr_t*)n);
  case EXPR_RETURN:    return retexpr(c, (retexpr_t*)n);
  case EXPR_MEMBER:    return member(c, (member_t*)n);

  case EXPR_BOOLLIT:
  case EXPR_INTLIT:
    return intlit(c, (intlit_t*)n);

  case EXPR_FLOATLIT:
    return floatlit(c, (floatlit_t*)n);

  case EXPR_STRLIT:
    return strlit(c, (strlit_t*)n);

  case EXPR_ARRAYLIT:
    return arraylit(c, (arraylit_t*)n);

  case EXPR_VAR:
  case EXPR_LET:
    return vardef(c, (local_t*)n);

  case EXPR_PARAM:
    return param(c, (local_t*)n);

  // TODO
  case EXPR_PREFIXOP:  // return prefixop(c, (unaryop_t*)n);
  case EXPR_POSTFIXOP: // return postfixop(c, (unaryop_t*)n);
  case EXPR_FOR:
    irval_t* v = push_TODO_val(c, c->b, type_void, "expr(%s)", nodekind_name(n->kind));
    seterr(c, ErrCanceled);
    return v;

  // We should never see these kinds of nodes
  case NODEKIND_COUNT:
  case NODE_BAD:
  case NODE_COMMENT:
  case NODE_UNIT:
  case STMT_TYPEDEF:
  case EXPR_FIELD:
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
  case TYPE_ARRAY:
  case TYPE_SLICE:
  case TYPE_MUTSLICE:
  case TYPE_FUN:
  case TYPE_PTR:
  case TYPE_REF:
  case TYPE_MUTREF:
  case TYPE_OPTIONAL:
  case TYPE_STRUCT:
  case TYPE_ALIAS:
  case TYPE_UNKNOWN:
  case TYPE_UNRESOLVED:
    break;
  }
  assertf(0, "unexpected node %s", nodekind_name(n->kind));
  panic("ircons: bad expr %u", n->kind);
}


static irunit_t* unit(ircons_t* c, unit_t* n) {
  irunit_t* u = mem_alloct(c->ir_ma, irunit_t);
  if UNLIKELY(!u)
    return out_of_mem(c), &bad_irunit;

  assert(c->unit == &bad_irunit);
  c->unit = u;

  for (u32 i = 0; i < n->children.len && c->compiler->errcount == 0; i++) {
    stmt_t* cn = n->children.v[i];
    {
      TRACE_NODE("stmt ", cn);
      switch (cn->kind) {
        case STMT_TYPEDEF:
          // ignore
          break;
        case EXPR_FUN:
          fun(c, (fun_t*)cn, NULL);
          break;
        default:
          assertf(0, "unexpected node %s", nodekind_name(cn->kind));
      }
    }

    // flush funqueue
    for (u32 i = 0; i < c->funqueue.len; i++) {
      fun_t* cn = c->funqueue.v[i];
      irfun_t** fp = (irfun_t**)map_lookup_ptr(&c->funm, cn);
      assertnotnull(fp);
      assertnotnull(*fp);
      TRACE_NODE("stmt ", cn);
      fun(c, cn, *fp);
    }
    c->funqueue.len = 0;
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

  c.deadset = assertnotnull( bitset_make(c.ma, BITSET_STACK_CAP) );

  if (!map_init(&c.funm, c.ma, MAX(n->children.len,1)*2)) {
    c.err = ErrNoMem;
    goto fail_funm;
  }

  if (!map_init(&c.vars, c.ma, 8)) {
    c.err = ErrNoMem;
    goto fail_vars;
  }

  irunit_t* u = unit(&c, n);

  // end
  *result = (u == &bad_irunit) ? NULL : u;

  ptrarray_dispose(&c.funqueue, c.ma);
  ptrarray_dispose(&c.dropstack, c.ma);
  ptrarray_dispose(&c.owners.entries, c.ma);
  bitset_dispose(c.deadset, c.ma);

  dispose_maparray(c.ma, &c.defvars);
  dispose_maparray(c.ma, &c.pendingphis);
  dispose_maparray(c.ma, &c.freemaps);

  map_dispose(&c.vars, c.ma);
fail_vars:
  map_dispose(&c.funm, c.ma);
fail_funm:
  return c.err;
}


err_t analyze(compiler_t* compiler, unit_t* unit, memalloc_t ir_ma) {
  irunit_t* u;
  err_t err = ircons(compiler, ir_ma, unit, &u);
  assertnotnull(u);
  if (compiler->opt_printir)
    dump_irunit(compiler, u);
  if (compiler->opt_genirdot)
    debug_graphviz(compiler, u);
  return err;
}
