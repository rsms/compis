// SPDX-License-Identifier: Apache-2.0
#include "c0lib.h"
#include "compiler.h"
#include <stdlib.h> // for debug_graphviz hack

#define TRACE_ANALYSIS


typedef struct {
  irfun_t*   f;
  irblock_t* b;
} fstate_t;

DEF_ARRAY_TYPE(fstate_t, fstatearray)
DEF_ARRAY_TYPE(map_t, maparray)

typedef enum {
  DROPKIND_LIVE,
  DROPKIND_DEAD,
  DROPKIND_BASE,
} dropkind_t;

typedef struct {
  dropkind_t kind;
  u32        id;
  union {
    irval_t* v;
    u32      base;
  };
} drop_t;

DEF_ARRAY_TYPE(drop_t, droparray)


typedef struct {
  compiler_t*   compiler;
  memalloc_t    ma;          // compiler->ma
  memalloc_t    ir_ma;       // allocator for ir data
  irunit_t*     unit;        // current unit
  irfun_t*      f;           // current function
  irblock_t*    b;           // current block
  fstatearray_t fstack;      // suspended function builds
  err_t         err;         // result of build process
  buf_t         tmpbuf[2];   // general-purpose scratch buffers
  map_t         funm;        // {fun_t* => irfun_t*} for breaking cycles
  map_t         vars;        // {sym_t => irval_t*} (moved to defvars by end_block)
  maparray_t    defvars;     // {[block_id] => map_t}
  maparray_t    pendingphis; // {[block_id] => map_t}
  maparray_t    freemaps;    // free map_t's (for defvars and pendingphis)

  struct {
    droparray_t entries;
    u32         base; // current scope's base index (e.g. entries.v[base])
    u32         maxid;
  } drops;

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


#define ID_CHAR(v_or_b) _Generic((v_or_b), \
  irval_t*:   'v', \
  irblock_t*: 'b' )


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
  #define TRACE_NODE(prefix, n) \
    trace_node(c, prefix, (node_t*)n); \
    c->traceindent++; \
    ircons_t* c2 __attribute__((__cleanup__(traceindent_decr),__unused__)) = c;

  static void traceindent_decr(void* cp) {
    (*(ircons_t**)cp)->traceindent--;
  }
#else
  #define trace(fmt, va...) ((void)0)
  #define trace_node(a,msg,n) ((void)0)
  #define TRACE_NODE(prefix, n) ((void)0)
#endif


#if 0
  static void debug_graphviz(const irunit_t* u, memalloc_t ma) {
    buf_t buf = buf_make(ma);
    buf_print(&buf, "echo '"
      "digraph G {\n"
      "  node [shape=record];\n"
    );
    for (u32 i = 0; i < u->functions.len; i++) {
      irfun_t* f = u->functions.v[i];
      if (u->functions.len > 1)
        buf_printf(&buf, "subgraph \"%s\" {\n", f->name);
      if (!irfmt_dot(&buf, f)) {
        fprintf(stderr, "(irfmt_dot failed)\n");
        buf_dispose(&buf);
        return;
      }
      if (u->functions.len > 1)
        buf_print(&buf, "}\n");
    }
    buf_print(&buf, "}' | graph-easy --from=dot --timeout=30 --as_boxart");
    buf_nullterm(&buf);
    system(buf.chars);
    buf_dispose(&buf);
  }
#else
  static void debug_graphviz(const irunit_t* u, memalloc_t ma) {
    buf_t buf = buf_make(ma);
    buf_print(&buf, "echo '"
      "digraph G {\n"
      "  overlap=false;\n"
      "  pad=0.2;\n"
      "  margin=0;\n"
      "  bgcolor=\"#171717\";\n"
      "  rankdir=LR; clusterrank=local;\n"
      "  size=\"9.6,6!\"; ratio=fill;\n"
      "  node [\n"
      "    color=white, shape=record, penwidth=2,\n"
      "    fontcolor=\"#ffffff\", fontname=Menlo, fontsize=14\n"
      "  ];\n"
      "  edge [\n"
      "    color=white, minlen=2,\n"
      "    fontcolor=\"#ffffff\", fontname=Menlo, fontsize=14\n"
      "  ];\n"
    );

    // note: backwards to make subgraphs order as expected
    for (u32 i = u->functions.len; i;) {
      irfun_t* f = u->functions.v[--i];
      if (u->functions.len > 1) {
        buf_printf(&buf,
          "subgraph cluster%p {\n"
          "penwidth=1; color=\"#ffffff44\"; margin=4;\n"
          "label=\"%s\"; labeljust=l;\n"
          "fontcolor=\"#ffffff44\"; fontname=Menlo; fontsize=12;\n"
          , f, f->name);
      }
      if (!irfmt_dot(&buf, f)) {
        fprintf(stderr, "(irfmt_dot failed)\n");
        buf_dispose(&buf);
        return;
      }
      if (u->functions.len > 1)
        buf_print(&buf, "}\n");
    }

    buf_print(&buf, "}' | dot -Tpng -oir-cfg.png");
    buf_nullterm(&buf);

    //dlog("dot: %s", buf.chars);

    dlog("ggraphviz...");
    system(buf.chars);
    dlog("graphviz... done");

    buf_dispose(&buf);
  }
#endif


static bool dump_irunit(const irunit_t* u, memalloc_t ma) {
  buf_t buf = buf_make(ma);
  if (!irfmt(&buf, u)) {
    fprintf(stderr, "(irfmt failed)\n");
    buf_dispose(&buf);
    return false;
  }
  fwrite(buf.chars, buf.len, 1, stderr);
  fputc('\n', stderr);
  buf_dispose(&buf);
  return true;
}


static bool dump_irfun(const irfun_t* f, memalloc_t ma) {
  buf_t buf = buf_make(ma);
  if (!irfmt_fun(&buf, f)) {
    fprintf(stderr, "(irfmt_fun failed)\n");
    buf_dispose(&buf);
    return false;
  }
  fwrite(buf.chars, buf.len, 1, stderr);
  fputc('\n', stderr);
  buf_dispose(&buf);
  return true;
}


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


ATTR_FORMAT(printf,3,4)
static void errorx(ircons_t* c, srcrange_t srcrange, const char* fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  report_diagv(c->compiler, srcrange, DIAG_ERR, fmt, ap);
  va_end(ap);
}

#define error(c, node_or_val, fmt, args...) \
  errorx((c), _Generic((node_or_val), \
    const irval_t*: (srcrange_t){.focus=(node_or_val)->loc}, \
    irval_t*:       (srcrange_t){.focus=(node_or_val)->loc}, \
    const node_t*:  node_srcrange((const node_t*)(node_or_val)), \
    node_t*:        node_srcrange((const node_t*)(node_or_val)), \
    const expr_t*:  node_srcrange((const node_t*)(node_or_val)), \
    expr_t*:        node_srcrange((const node_t*)(node_or_val)) \
  ), (fmt), ##args)


// static type_t* basetype(type_t* t) {
//   // unwrap optional and ref
//   assertnotnull(t);
//   if (t->kind == TYPE_OPTIONAL)
//     t = assertnotnull(((opttype_t*)t)->elem);
//   if (t->kind == TYPE_REF)
//     t = assertnotnull(((reftype_t*)t)->elem);
//   return t;
// }


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


#define push_TODO_val(c, b, type, fmt, args...) ( \
  dlog("TODO_val " fmt, ##args), \
  comment((c), pushval((c), (b), OP_NOOP, (srcloc_t){0}, (type)), "TODO") \
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


static bool isrvalue(const void* expr) {
  assert(node_isexpr(expr));
  return ((const expr_t*)expr)->flags & EX_RVALUE;
}


#define assert_can_own(v_or_b) assertf(\
  _Generic((v_or_b), \
    irval_t*:   type_isptr(((irval_t*)(v_or_b))->type), \
    irblock_t*: true ), \
  "%c%u is not an owner type", ID_CHAR(v_or_b), (v_or_b)->id)


// // move_owner transfers ownership of value, from src to dst, if src is ptrtype.
// //   void move_owner(ircons_t* c, irval_t*   dst, irval_t*   src)  v <- v
// //   void move_owner(ircons_t* c, irblock_t* dst, irval_t*   src)  b <- v
// //   void move_owner(ircons_t* c, irblock_t* dst, irblock_t* src)  b <- b
// //   void move_owner(ircons_t* c, irval_t*   dst, irblock_t* src)  v <- b
// #define move_owner(c, dst, src) ( \
//   ((src)->flags & IR_OWNER) ? ( \
//     trace("move owner: %c%u -> %c%u", \
//       ID_CHAR(src), (src)->id, ID_CHAR(dst), (dst)->id), \
//     assert_can_own(dst), \
//     ((src)->flags &= ~IR_OWNER), \
//     ((dst)->flags |= IR_OWNER) ) : \
//   ((void)0) )


static void drops_reset(ircons_t* c) {
  c->drops.maxid = 0;
  c->drops.base = 0;
  droparray_clear(&c->drops.entries);
}


static void drops_scope_enter(ircons_t* c) {
  trace("\e[1;34m%s\e[0m", __FUNCTION__);
  drop_t* d = droparray_alloc(&c->drops.entries, c->ma, 1);
  if UNLIKELY(!d)
    return out_of_mem(c);
  d->kind = DROPKIND_BASE;
  d->base = c->drops.base;
  c->drops.base = c->drops.entries.len - 1;
}


static void drops_scope_leave(ircons_t* c) {
  trace("\e[1;34m%s\e[0m", __FUNCTION__);
  c->drops.entries.len = c->drops.base;
  c->drops.base = c->drops.entries.v[c->drops.base].base;
}


static void drops_mark(ircons_t* c, irval_t* v, dropkind_t kind) {
  trace("\e[1;34m%s\e[0m" " %s v%u", __FUNCTION__,
    (kind == DROPKIND_LIVE) ? "live" : "dead", v->id);
  assert(kind != DROPKIND_BASE);
  drop_t* d = droparray_alloc(&c->drops.entries, c->ma, 1);
  if UNLIKELY(!d)
    return out_of_mem(c);
  d->kind = kind;
  d->v = v;
  d->id = v->id;
  c->drops.maxid = MAX(c->drops.maxid, v->id);
}


drop_t* nullable drops_lookup(ircons_t* c, u32 id) {
  trace("%s v%u", __FUNCTION__, id);
  u32 base = c->drops.base;
  for (u32 i = c->drops.entries.len; i;) {
    drop_t* d = &c->drops.entries.v[--i];
    if (i == base) {
      assert(d->kind == DROPKIND_BASE);
      base = d->base;
    } else if (d->id == id) {
      return d;
    }
  }
  trace("  not found");
  return NULL;
}


static void drops_gen(ircons_t* c, srcloc_t loc, u32 maxdepth) {
  if (c->drops.entries.len == 0)
    return;

  bitset_t* seen = bitset_make(c->ma, (usize)c->drops.maxid + 1lu);
  if UNLIKELY(!seen)
    return out_of_mem(c);

  u32 base = c->drops.base;
  for (u32 i = c->drops.entries.len; i;) {
    drop_t* d = &c->drops.entries.v[--i];
    if (i == base) {
      assert(d->kind == DROPKIND_BASE);
      if (maxdepth == 0)
        break;
      maxdepth--;
      trace("  base [%u] -> [%u]", base, d->base);
      base = d->base;
      continue;
    }
    if (d->kind == DROPKIND_DEAD) {
      trace("  dead v%d", d->id);
      bitset_add(seen, d->id);
      continue;
    }

    if (bitset_has(seen, d->id))
      continue;
    bitset_add(seen, d->id);
    trace("  %s v%d", d->kind == DROPKIND_LIVE ? "live" : "dead", d->id);
    if (d->kind == DROPKIND_LIVE) {
      irval_t* v = pushval(c, c->b, OP_DROP, loc, type_void);
      pusharg(v, d->v);
      v->var.src = d->v->var.dst;
    }
  }

  bitset_dispose(seen, c->ma);
}


static void drops_gen_exit(ircons_t* c, srcloc_t loc) {
  trace("\e[1;34m%s\e[0m", __FUNCTION__);
  drops_gen(c, loc, U32_MAX);
}


static void drops_gen_scope(ircons_t* c, srcloc_t loc) {
  trace("\e[1;34m%s\e[0m", __FUNCTION__);
  drops_gen(c, loc, 0);
}


static void move_owner(ircons_t* c, irval_t* nullable new_owner, irval_t* curr_owner) {
  drops_mark(c, curr_owner, DROPKIND_DEAD);
  if (new_owner) {
    drops_mark(c, new_owner, DROPKIND_LIVE);
    trace("\e[1;33m" "move owner: v%u -> v%u" "\e[0m", curr_owner->id, new_owner->id);
  } else {
    trace("\e[1;33m" "move owner: v%u -> escape" "\e[0m", curr_owner->id);
  }
}


static irval_t* move(ircons_t* c, irval_t* rvalue) {
  irval_t* v = pushval(c, c->b, OP_MOVE, rvalue->loc, rvalue->type);
  pusharg(v, rvalue);
  move_owner(c, v, rvalue);
  return v;
}


static irval_t* reference(ircons_t* c, irval_t* rvalue) {
  op_t op = ((reftype_t*)rvalue->type)->ismut ? OP_BORROW_MUT : OP_BORROW;
  irval_t* v = pushval(c, c->b, op, rvalue->loc, rvalue->type);
  pusharg(v, rvalue);
  return v;
}


static irval_t* move_or_copy(ircons_t* c, irval_t* rvalue) {
  irval_t* v = rvalue;
  if (type_ismove(rvalue->type)) {
    v = move(c, rvalue);
  } else if (type_isref(rvalue->type)) {
    v = reference(c, rvalue);
  }
  v->var.src = rvalue->var.dst;
  return v;
}


//—————————————————————————————————————————————————————————————————————————————————————


static irval_t* var_read_recursive(ircons_t*, irblock_t*, sym_t, type_t*, srcloc_t);


// typedef struct {
//   irval_t* v;
//   irflag_t flags;
// } var_t;


static void var_write_map(ircons_t* c, map_t* vars, sym_t name, irval_t* v) {
  irval_t** vp = (irval_t**)map_assign_ptr(vars, c->ma, name);
  if UNLIKELY(!vp)
    return out_of_mem(c);

  // var_t* var = mem_alloct(c->ma, var_t);
  // if UNLIKELY(!var)
  //   return out_of_mem(c);
  // var->v = v;
  // var->flags = ...

  // irval_t* prev = *vp;
  *vp = v;

  // if (type_isowner(v->type)) {
  //   if (prev) {
  //     trace(">> transfer ownership v%u -> v%u", prev->id, v->id);
  //     drops_del(c, prev);
  //   } else {
  //     trace(">> establish ownership v%u", v->id);
  //   }
  //   if UNLIKELY(!ptrarray_push(&c->drops, c->ma, v))
  //     out_of_mem(c);
  // }
}


static irval_t* var_read_map(
  ircons_t* c, irblock_t* b, map_t* vars, sym_t name, type_t* type, srcloc_t loc)
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
  ircons_t* c, irblock_t* b, sym_t name, type_t* type, srcloc_t loc)
{
  //trace("%s %s in b%u", __FUNCTION__, name, b->id);
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

  //trace("%s in b%u for %s", __FUNCTION__, b->id, name);

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
  //trace("%s %s in b%u", __FUNCTION__, name, b->id);

  irval_t* v;

  if ((b->flags & IR_SEALED) == 0) {
    // incomplete CFG
    //trace("  block not yet sealed");
    v = pushval(c, b, OP_PHI, loc, type);
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


static void set_control(ircons_t* c, irblock_t* b, irval_t* nullable v) {
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
  for (const mapent_t* e = map_it(&c->vars); map_itnext(&c->vars, &e); ) {
    irval_t* v = e->value;
    trace("  %s %s = v%u", (const char*)e->key, fmtnode(0, v->type), v->id);
  }

  map_t* vars = assign_block_map(c, &c->defvars, b->id);
  if UNLIKELY(!vars)
    return;
  *vars = c->vars;

  // replace c.vars with a new map
  if UNLIKELY(!alloc_map(c, &c->vars))
    out_of_mem(c);
}


static irblock_t* end_block(ircons_t* c) {
  // transfer live locals and seals c->b if needed
  trace("%s b%u", __FUNCTION__, c->b->id);
  #ifdef TRACE_ANALYSIS
  // c->traceindent++;
  #endif

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

  // // dump state of vars
  // #ifdef TRACE_ANALYSIS
  // trace("defvars");
  // for (u32 bi = 0; bi < c->defvars.len; bi++) {
  //   map_t* vars = &c->defvars.v[bi];
  //   if (vars->len == 0)
  //     continue;
  //   trace("  b%u", bi);
  //   for (const mapent_t* e = map_it(vars); map_itnext(vars, &e); ) {
  //     sym_t name = e->key;
  //     irval_t* v = e->value;
  //     trace("    %s %s = v%u", name, fmtnode(0, v->type), v->id);
  //   }
  // }
  // c->traceindent--;
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


static irval_t* load_expr(ircons_t* c, expr_t* n) {
  trace("\e[1;35m" "load %s %s" "\e[0m", nodekind_fmt(n->kind), fmtnode(0, n));
  if (!isrvalue(n))
    trace("\e[1;31m" "  AST not marked as rvalue" "\e[0m");

  irval_t* v = expr(c, n);

  switch (n->kind) {
  case EXPR_ID: {
    idexpr_t* id = (idexpr_t*)n;
    if (node_islocal(id->ref)) {
      local_t* local = (local_t*)id->ref;
      drop_t* d = drops_lookup(c, v->id);
      if (d && d->kind != DROPKIND_LIVE) {
        error(c, n, "use of dead value; moved away");
      }
      dlog("drops_lookup(v%u) => %s", v->id,
        d ? (d->kind == DROPKIND_LIVE ? "live" : "dead") : "?");
      dlog("TODO %s check for uninitialized ref or ptr var", nodekind_name(n->kind));
      dlog("TODO %s check for dead owner (moved out)", nodekind_name(n->kind));
    }
    break;
  }
  default:
    dlog("TODO %s", nodekind_name(n->kind));
  }

  return v;
}


static irval_t* idexpr(ircons_t* c, idexpr_t* n) {
  assertnotnull(n->ref);
  assertf(node_islocal(n->ref), "%s", nodekind_name(n->ref->kind));
  local_t* local = (local_t*)n->ref;
  return var_read(c, local->name, local->type, local->loc);
}


static irval_t* assign_local(ircons_t* c, local_t* dst, irval_t* v) {
  v = move_or_copy(c, v);
  v->var.dst = dst->name;
  var_write(c, dst->name, v);
  return v;
}


static irval_t* vardef(ircons_t* c, local_t* n) {
  irval_t* v;
  if (n->init) {
    v = load_expr(c, n->init);
  } else {
    v = pushval(c, c->b, OP_ZERO, n->loc, n->type);
  }
  if (n->name == sym__)
    return move_or_copy(c, v);
  return assign_local(c, n, v);
}


static irval_t* assign(ircons_t* c, binop_t* n) {
  irval_t* v = load_expr(c, n->right);
  idexpr_t* id = (idexpr_t*)n->left;
  assertf(id->kind == EXPR_ID, "TODO %s", nodekind_name(id->kind));
  if (id->name == sym__)
    return move_or_copy(c, v);
  assert(node_islocal(id->ref));
  return assign_local(c, (local_t*)id->ref, v);
}


static irval_t* retexpr(ircons_t* c, retexpr_t* n) {
  irval_t* v = n->value ? load_expr(c, n->value) : NULL;
  c->b->kind = IR_BLOCK_RET;
  move_owner(c, NULL, v);
  set_control(c, c->b, v);
  drops_gen_exit(c, n->loc);
  return v ? v : &bad_irval;
}


static irval_t* blockexpr0(ircons_t* c, block_t* n) {
  if (n->children.len == 0) {
    if (isrvalue(n))
      return pushval(c, c->b, OP_ZERO, n->loc, n->type);
    return &bad_irval;
  }
  u32 lastrval = (n->children.len-1) + (u32)!isrvalue(n);
  for (u32 i = 0; i < n->children.len; i++) {
    expr_t* cn = n->children.v[i];
    if (i == lastrval)
      return load_expr(c, cn);
    expr(c, cn);
    if (cn->kind == EXPR_RETURN)
      break;
  }
  return &bad_irval;
}


static irval_t* blockexpr(ircons_t* c, block_t* n) {
  TRACE_NODE("expr ", n);
  return blockexpr0(c, n);
}


static irval_t* blockexpr1(ircons_t* c, block_t* n) {
  #define CREATE_BB_FOR_BLOCK

  #ifdef CREATE_BB_FOR_BLOCK
    irblock_t* prevb = end_block(c);
    prevb->kind = IR_BLOCK_CONT;

    irblock_t* b = mkblock(c, c->f, IR_BLOCK_CONT, n->loc);
    irblock_t* contb = mkblock(c, c->f, IR_BLOCK_CONT, n->loc);

    prevb->succs[0] = b;
    b->preds[0] = prevb;
    b->succs[0] = contb;
    contb->preds[0] = b;

    start_block(c, b);
    seal_block(c, b);
  #endif

  drops_scope_enter(c);

  irval_t* v = blockexpr0(c, n);
  if (isrvalue(n))
    move_owner(c, NULL, v);

  drops_gen_scope(c, n->loc);
  drops_scope_leave(c);

  #ifdef CREATE_BB_FOR_BLOCK
    b = end_block(c);
    start_block(c, contb);
    seal_block(c, contb);
  #endif

  return v;
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
  irfun_t* f = c->f;

  // generate control condition
  irval_t* control = load_expr(c, n->cond);
  assert(control->type == type_bool);

  // end predecessor block (leading up to and including "if")
  irblock_t* ifb = end_block(c);
  ifb->kind = IR_BLOCK_IF;
  set_control(c, ifb, control);

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
  drops_scope_enter(c);
  irval_t* thenv = blockexpr(c, n->thenb);
  drops_scope_leave(c);
  thenb = end_block(c);

  irval_t* elsev;

  // begin "else" block if there is one
  if (n->elseb) {
    trace("if \"else\" block");

    // allocate "cont" block; the block following both thenb and elseb
    // TODO: can we move this past the "thenb->kind == IR_BLOCK_RET" check?
    u32 contb_index = f->blocks.len;
    irblock_t* contb = mkblock(c, f, IR_BLOCK_CONT, n->loc);
    comment(c, contb, fmttmp(c, 0, "b%u.cont", ifb->id));

    // begin "else" block
    comment(c, elseb, fmttmp(c, 0, "b%u.else", ifb->id));
    elseb->preds[0] = ifb; // else <- if
    start_block(c, elseb);
    seal_block(c, elseb);
    drops_scope_enter(c);
    elsev = blockexpr(c, n->elseb);
    drops_scope_leave(c);

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

    if (isrvalue(n)) {
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
  if (elseb->kind == IR_BLOCK_RET || !isrvalue(n))
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


// postorder_dfs provides a DFS postordering of blocks in f
static void postorder_dfs(ircons_t* c, irfun_t* f, irblock_t** order) {
  if (f->blocks.len == 0)
    return;

  // track which blocks we have visited to break cycles, using a bitset of block IDs
  u32 idcount = f->bidgen;
  bitset_t* visited = bitset_make(c->ma, idcount);
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


typedef struct {
  u32 r, w, len, cap;
  void** entries;
} ptrqueue_t;


static bool ptrqueue_init(ptrqueue_t* q, memalloc_t ma, u32 cap) {
  q->entries = mem_alloctv(ma, void*, cap);
  if (!q->entries)
    return false;
  q->cap = cap;
  return true;
}


static void ptrqueue_dispose(ptrqueue_t* q, memalloc_t ma) {
  mem_freetv(ma, q->entries, q->cap);
}


static bool ptrqueue_isempty(ptrqueue_t* q) { return q->len == 0; }


static void ptrqueue_clear(ptrqueue_t* q) {
  q->len = 0;
  q->r = 0;
  q->w = 0;
}


static void ptrqueue_push(ptrqueue_t* q, void* value) {
  assertf(q->len < q->cap, "buffer overflow");
  q->entries[q->w++] = value;
  q->len++;
  if (q->w == q->cap)
    q->w = 0;
}


static void* ptrqueue_pop(ptrqueue_t* q) {
  assertf(q->len > 0, "empty");
  void* v = q->entries[q->r++];
  q->len--;
  if (q->r == q->cap)
    q->r = 0;
  return v;
}


static void la_walk(
  ircons_t* c, irfun_t* f, bitset_t* visited, ptrarray_t* g, irval_t* v)
{
  // Walk
  //
  // The core algorithm works by traversing G in a depth-first search manner. This
  // is done by a function named Walk (algorithm 4) that takes the graph G and a
  // start node s as parameters. The initial vertex s is the one related to the
  // value currently being heap-allocated and which is going to be analyzed in
  // order to determine if it can be instead stack-allocated. For each node u
  // reached, a function V isit is applied to verify if u must escape or not. This
  // function is detailed in algorithm 5.
  //
  ptrarray_t workstack = {0};
  bitset_clear(visited);

  dlog("la_walk v%u", v->id);

  for (u32 i = 0; i < v->parents.len; i++) {
    irval_t* parentval = v->parents.v[i];
    ptrarray_push(&workstack, c->ma, parentval);
  }

  while (workstack.len > 0) {
    irval_t* v = ptrarray_pop(&workstack);
    if (bitset_has(visited, v->id))
      continue;
    dlog("TODO: visit v%u", v->id);
    bitset_add(visited, v->id);

    // a

    // b

    for (u32 i = 0; i < v->parents.len; i++) {
      irval_t* parentval = v->parents.v[i];
      ptrarray_push(&workstack, c->ma, parentval);
    }
  }

  ptrarray_dispose(&workstack, c->ma);
}


static void la_propagate_region(
  ircons_t* c, irfun_t* f, bitset_t* visited, ptrarray_t* g)
{
  ptrqueue_t q;
  if UNLIKELY(!ptrqueue_init(&q, c->ma, g->len))
    return out_of_mem(c);

  for (u32 i = 0; i < g->len; i++) {
    irval_t* v = g->v[i];
    if ((v->flags & IR_LA_OUTSIDE) == 0)
      continue;
    ptrqueue_clear(&q);
    ptrqueue_push(&q, v);
    for (;;) {
      // for each adjacent node w of v not visited yet do ...
      for (u32 j = 0; j < v->edges.len; j++) {
        irval_t* w = v->edges.v[j];
        if (bitset_has(visited, w->id))
          continue;
        w->flags |= IR_LA_OUTSIDE;
        dlog("v%u -> outside", w->id);
        ptrqueue_push(&q, w);
        bitset_add(visited, w->id);
      }
      if (ptrqueue_isempty(&q))
        break;
      v = ptrqueue_pop(&q);
    }
  }

  ptrqueue_dispose(&q, c->ma);
}


static void la_build_graph(ircons_t* c, irfun_t* f) {
  ptrarray_t g = {0};
  ptrarray_reserve(&g, c->ma, 16);
  u32 maxid = 0;

  for (u32 i = 0; i < f->blocks.len; i++) {
    irblock_t* b = f->blocks.v[i];
    for (u32 j = 0; j < b->values.len; j++) {
      irval_t* v = b->values.v[j];
      maxid = MAX(maxid, v->id);

      if (type_isowner(v->type)) {
        v->flags |= IR_LA_OUTSIDE;
        dlog("v%u : outside", v->id);
      } else {
        dlog("v%u : inside", v->id);
      }
      ptrarray_push(&g, c->ma, v);

      for (u32 k = 0; k < v->argc; k++) {
        irval_t* arg = v->argv[k];
        if UNLIKELY(
          !ptrarray_push(&arg->parents, c->ir_ma, v) ||
          !ptrarray_push(&v->edges, c->ir_ma, arg) )
        {
          goto end;
        }
      }
    }
  }

  // create a bitset for tracking visited values by ID
  bitset_t* visited = bitset_make(c->ma, maxid + 1);
  if UNLIKELY(!visited) {
    out_of_mem(c);
    goto end;
  }

  la_propagate_region(c, f, visited, &g);

  for (u32 i = 0; i < g.len; i++) {
    irval_t* v = g.v[i];
    if (v->flags & IR_LA_OUTSIDE)
      la_walk(c, f, visited, &g, v);
  }

  // check for oom
  if (!ptrarray_reserve(&g, c->ma, 1))
    out_of_mem(c);

  bitset_dispose(visited, c->ma);
end:
  ptrarray_dispose(&g, c->ma);
}


static void analyze_ownership(ircons_t* c, irfun_t* f) {
  dump_irfun(f, c->ma);
  trace("analyze_ownership");

  la_build_graph(c, f);

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
    panic("TODO implement queue of functions");
  }

  c->f = f;
  drops_reset(c);

  // allocate entry block
  irblock_t* entryb = mkblock(c, f, IR_BLOCK_CONT, n->loc);
  start_block(c, entryb);
  seal_block(c, entryb); // entry block has no predecessors

  // enter function scope
  drops_scope_enter(c);

  // define arguments
  for (u32 i = 0; i < n->params.len; i++) {
    local_t* param = n->params.v[i];
    if (param->name == sym__)
      continue;
    irval_t* v = pushval(c, c->b, OP_ARG, param->loc, param->type);
    v->aux.i32val = i;
    v->var.dst = param->name;
    comment(c, v, param->name);

    if (type_isowner(param->type)) {
      v->flags |= IR_OWNER;
      drops_mark(c, v, DROPKIND_LIVE);
    }

    var_write(c, param->name, v);
  }

  // build body
  irval_t* body = blockexpr(c, n->body);

  // end last block
  // note: if the block ended with a "return" statement, b->kind is already BLOCK_RETe
  if (c->b->kind != IR_BLOCK_RET) {
    c->b->kind = IR_BLOCK_RET;
    if (((funtype_t*)n->type)->result != type_void && body != &bad_irval) {
      // implicit return
      move_owner(c, NULL, body);
      set_control(c, c->b, body);
    }
    drops_gen_exit(c, n->body->loc);
  }
  end_block(c);

  // leave function scope
  drops_scope_leave(c);

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

  //analyze_ownership(c, f);

end:
  c->f = &bad_irfun;
  return f;
}


static irval_t* expr(ircons_t* c, expr_t* n) {
  TRACE_NODE("expr ", n);

  switch ((enum nodekind)n->kind) {
  case EXPR_BLOCK:     return blockexpr1(c, (block_t*)n);
  case EXPR_BINOP:     return binop(c, (binop_t*)n);
  case EXPR_ASSIGN:    return assign(c, (binop_t*)n);
  case EXPR_ID:        return idexpr(c, (idexpr_t*)n);
  case EXPR_RETURN:    return retexpr(c, (retexpr_t*)n);
  case EXPR_FLOATLIT:  return floatlit(c, (floatlit_t*)n);
  case EXPR_IF:        return ifexpr(c, (ifexpr_t*)n);

  case EXPR_BOOLLIT:
  case EXPR_INTLIT:
    return intlit(c, (intlit_t*)n);

  case EXPR_VAR:
  case EXPR_LET:
    return vardef(c, (local_t*)n);

  // TODO
  case EXPR_FUN:       // return funexpr(c, (fun_t*)n);
  case EXPR_CALL:      // return call(c, (call_t*)n);
  case EXPR_MEMBER:    // return member(c, (member_t*)n);
  case EXPR_DEREF:     // return deref(c, (unaryop_t*)n);
  case EXPR_PREFIXOP:  // return prefixop(c, (unaryop_t*)n);
  case EXPR_POSTFIXOP: // return postfixop(c, (unaryop_t*)n);
  case EXPR_FOR:
    c->err = ErrCanceled;
    return push_TODO_val(c, c->b, type_void, "expr(%s)", nodekind_name(n->kind));

  // We should never see these kinds of nodes
  case NODEKIND_COUNT:
  case NODE_BAD:
  case NODE_COMMENT:
  case NODE_UNIT:
  case STMT_TYPEDEF:
  case EXPR_FIELD:
  case EXPR_PARAM:
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
    TRACE_NODE("stmt ", n);
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

  droparray_dispose(&c.drops.entries, c.ma);

  *result = u == &bad_irunit ? NULL : u;
  return c.err;
}


err_t analyze2(compiler_t* compiler, memalloc_t ir_ma, unit_t* unit) {
  irunit_t* u;
  err_t err = ircons(compiler, ir_ma, unit, &u);
  assertnotnull(u);
  dump_irunit(u, compiler->ma);
  debug_graphviz(u, compiler->ma);
  return ErrCanceled; // XXX
  return err;
}
