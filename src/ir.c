// SPDX-License-Identifier: Apache-2.0
#include "c0lib.h"
#include "compiler.h"
#include <stdlib.h> // for debug_graphviz hack

#define TRACE_ANALYSIS


DEF_ARRAY_TYPE(map_t, maparray)

typedef enum {
  DROPKIND_LIVE,
  DROPKIND_DEAD,
  DROPKIND_BASE,
} dropkind_t;

typedef struct {
  dropkind_t kind;
  u32        id;
  irval_t*   v;
} drop_t;

DEF_ARRAY_TYPE(drop_t, droparray)


typedef struct {
  compiler_t*   compiler;
  memalloc_t    ma;          // compiler->ma
  memalloc_t    ir_ma;       // allocator for ir data
  irunit_t*     unit;        // current unit
  irfun_t*      f;           // current function
  irblock_t*    b;           // current block
  err_t         err;         // result of build process
  buf_t         tmpbuf[2];   // general-purpose scratch buffers
  map_t         funm;        // {fun_t* => irfun_t*} for breaking cycles
  map_t         vars;        // {sym_t => irval_t*} (moved to defvars by end_block)
  maparray_t    defvars;     // {[block_id] => map_t}
  maparray_t    pendingphis; // {[block_id] => map_t}
  maparray_t    freemaps;    // free map_t's (for defvars and pendingphis)
  bitset_t*     deadset;

  struct {
    ptrarray_t entries;
    u32        base;   // current scope's base index
  } owners;

  struct {
    droparray_t entries;
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
  static void _traceindent_decr(void* cp) { (*(ircons_t**)cp)->traceindent--; }

  #define TRACE_SCOPE() \
    c->traceindent++; \
    ircons_t* c2 __attribute__((__cleanup__(_traceindent_decr),__unused__)) = c

  #define TRACE_NODE(prefix, n) \
    trace_node(c, prefix, (node_t*)n); \
    TRACE_SCOPE();
#else
  #define trace(fmt, va...) ((void)0)
  #define trace_node(a,msg,n) ((void)0)
  #define TRACE_NODE(prefix, n) ((void)0)
  #define TRACE_INDENT_INCR_SCOPE() ((void)0)
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
    #define DOT_FONT "fontname=\"JetBrains Mono NL, Menlo, Courier, monospace\";"
    // buf_print(&buf, "echo '");
    buf_print(&buf,
      "digraph G {\n"
      "  overlap=false;\n"
      "  pad=0.2;\n"
      "  margin=0;\n"
      "  bgcolor=\"#171717\";\n"
      "  rankdir=TB; clusterrank=local;\n"
      "  size=\"9.6,8!\";\n" //"ratio=fill;\n"
      "  node [\n"
      "    color=white, shape=record, penwidth=1,\n"
      "    fontcolor=\"#ffffff\"; " DOT_FONT " fontsize=14\n"
      "  ];\n"
      "  edge [\n"
      "    color=white, minlen=2,\n"
      "    fontcolor=\"#ffffff\"; " DOT_FONT " fontsize=14\n"
      "  ];\n"
    );

    // exclude empty (declaration-only) functions
    u32 nfuns = 0;
    for (u32 i = u->functions.len; i;)
      nfuns += (u32)( ((irfun_t*)u->functions.v[--i])->blocks.len != 0 );

    // note: backwards to make subgraphs order as expected
    for (u32 i = u->functions.len; i;) {
      irfun_t* f = u->functions.v[--i];
      if (f->blocks.len == 0)
        continue;
      if (nfuns > 1) {
        buf_printf(&buf,
          "subgraph cluster%p {\n"
          "penwidth=1; color=\"#ffffff44\"; margin=4;\n"
          "label=\"%s\"; labeljust=l;\n"
          "fontcolor=\"#ffffff44\"; " DOT_FONT " fontsize=12;\n"
          , f, f->name);
      }
      if (!irfmt_dot(&buf, f)) {
        fprintf(stderr, "(irfmt_dot failed)\n");
        buf_dispose(&buf);
        return;
      }
      if (nfuns > 1)
        buf_print(&buf, "}\n");
    }

    buf_print(&buf, "}");

    err_t err = writefile("ir.dot", 0664, buf_slice(buf));
    if (err) {
      fprintf(stderr, "failed to write file ir.dot: %s", err_str(err));
      goto end;
    }

    buf_clear(&buf);
    buf_print(&buf, "dot -Tpng -oir.png ir.dot");
    buf_nullterm(&buf);

    // buf_print(&buf, "' | dot -Tpng -oir-cfg.png");
    // buf_nullterm(&buf);
    // // dlog("dot:\n———————————\n%s\n———————————", buf.chars);

    dlog("graphviz...");
    system(buf.chars);
  end:
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


// const srcrange_t to_srcrange(ircons_t*, T origin)
// where T is one of: irval_t* | node_t* | expr_t* | srcrange_t
#define to_srcrange(origin) ({ \
  __typeof__(origin)* __tmp = &origin; \
  const srcrange_t __s = _Generic(__tmp, \
    const srcrange_t*: *(const srcrange_t*)__tmp, \
          srcrange_t*: *(const srcrange_t*)__tmp, \
    const irval_t**:   (srcrange_t){.focus=(*(const irval_t**)__tmp)->loc}, \
          node_t**:    node_srcrange(*(const node_t**)__tmp), \
    const node_t**:    node_srcrange(*(const node_t**)__tmp), \
          expr_t**:    node_srcrange(*(const node_t**)__tmp), \
    const expr_t**:    node_srcrange(*(const node_t**)__tmp) \
  ); \
  __s; \
})

// void diag(ircons_t*, T origin, diagkind_t diagkind, const char* fmt, ...)
// where T is one of: irval_t* | node_t* | expr_t* | srcrange_t
#define diag(c, origin, diagkind, fmt, args...) \
  report_diag((c)->compiler, to_srcrange(origin), (diagkind), (fmt), ##args)

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


static void deadset_del(ircons_t* c, bitset_t** bsp, u32 id) {
  if UNLIKELY(!bitset_ensure_cap(bsp, c->ma, id+1))
    return out_of_mem(c);
  bitset_del(*bsp, id);
}


static srcrange_t find_moved_srcrange(ircons_t* c, u32 id) {
  // only used for diagnostics so doesn't have to be fast
  for (u32 bi = c->f->blocks.len; bi != 0;) {
    irblock_t* b = c->f->blocks.v[--bi];
    for (u32 vi = b->values.len; vi != 0;) {
      irval_t* v = b->values.v[--vi];
      if (v->op != OP_MOVE)
        continue;
      for (u32 ai = v->argc; ai != 0;) {
        irval_t* a = v->argv[--ai];
        if (a->id == id) {
          dlog("found v%u", v->id);
          return (srcrange_t){ .focus = v->loc };
        }
      }
    }
  }
  return (srcrange_t){0}; // not found
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


static map_t* assign_block_map(ircons_t* c, maparray_t* a, u32 block_id) {
  static map_t last_resort_map = {0};
  if (a->len <= block_id) {
    // fill holes
    u32 nholes = (block_id - a->len) + 1;
    map_t* p = maparray_alloc(a, c->ma, nholes);
    if UNLIKELY(!p) {
      return out_of_mem(c), &last_resort_map;
    }
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
  return ((const expr_t*)expr)->flags & EX_RVALUE;
}


#define assert_can_own(v_or_b) assertf(\
  _Generic((v_or_b), \
    irval_t*:   type_isptr(((irval_t*)(v_or_b))->type), \
    irblock_t*: true ), \
  "%c%u is not an owner type", ID_CHAR(v_or_b), (v_or_b)->id)


static void drops_reset(ircons_t* c) {
  c->drops.maxid = 0;
  droparray_clear(&c->drops.entries);
  bitset_clear(c->deadset);
}


static u32 drops_scope_enter(ircons_t* c) {
  trace("\e[1;34m%s\e[0m", __FUNCTION__);
  return c->drops.entries.len;
}


static void drops_scope_leave(ircons_t* c, u32 scope_base) {
  trace("\e[1;34m%s\e[0m", __FUNCTION__);
  c->drops.entries.len = scope_base;
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
  trace("\e[1;34m%s\e[0m" " v%u", __FUNCTION__, id);
  if (id <= c->drops.maxid) {
    for (u32 i = c->drops.entries.len; i;) {
      drop_t* d = &c->drops.entries.v[--i];
      if (d->id == id)
        return d;
    }
  }
  trace("  not found");
  return NULL;
}


static void drop(ircons_t* c, irval_t* val_to_drop, srcloc_t loc) {
  irval_t* v = pushval(c, c->b, OP_DROP, loc, type_void);
  pusharg(v, val_to_drop);
  v->var.src = val_to_drop->var.dst;
}


static void drops_unwind(ircons_t* c, u32 scope_base, srcloc_t loc) {
  return; // XXX

  trace("\e[1;34m%s\e[0m scope_base=%u", __FUNCTION__, scope_base);

  assert(c->drops.entries.len >= scope_base);
  if (c->drops.entries.len == scope_base)
    return;

  usize idcap = (usize)c->drops.maxid + 1lu;

  if (!bitset_ensure_cap(&c->deadset, c->ma, idcap))
    return out_of_mem(c);

  bitset_t* liveset = bitset_make(c->ma, idcap);
  if (!liveset)
    return out_of_mem(c);

  for (u32 i = c->drops.entries.len; i > scope_base;) {
    drop_t* d = &c->drops.entries.v[--i];
    if (d->kind == DROPKIND_DEAD) {
      trace("  v%d dead", d->id);
      bitset_add(c->deadset, d->id);
    } else if (!bitset_has(liveset, d->id) && !bitset_has(c->deadset, d->id)) {
      trace("  v%d LIVE", d->id);
      bitset_add(liveset, d->id);
      if (d->kind == DROPKIND_LIVE)
        drop(c, d->v, loc);
    }
  }

  bitset_dispose(liveset, c->ma);
}


static void drops_unwind_scope(ircons_t* c, u32 scope_base, srcloc_t loc) {
  drops_unwind(c, scope_base, loc);
}


static void drops_unwind_exit(ircons_t* c, srcloc_t loc) {
  drops_unwind(c, 0, loc);
}


static u32 drops_since_lastindex(ircons_t* c, bitset_t* deadset1, bitset_t* deadset2) {
  // detect if values which lost ownership since entry_deadset.
  // returns index+1 of first (top of stack) drops.entries entry.
  u32 i = c->drops.entries.len;
  for (; i > 0;) {
    drop_t* d = &c->drops.entries.v[--i];
    if (bitset_has(deadset2, d->id) && !bitset_has(deadset1, d->id))
      return i + 1;
  }
  return 0;
}


// static void drops_since_gen(
//   ircons_t* c, bitset_t* deadset1, const bitset_t* deadset2, u32 i, srcloc_t loc)
// {
//   // generate drops for values which lost ownership since deadset1 until deadset2
//   while (i > 0) {
//     drop_t* d = &c->drops.entries.v[--i];
//     //dlog("v%u (%s) ...", d->id, d->kind == DROPKIND_LIVE ? "live" : "dead");
//     if (bitset_has(deadset2, d->id) && !bitset_has(deadset1, d->id)) {
//       trace("  v%u lost ownership", d->id);
//       assert(deadset1->cap > d->id);

//       // mark as dead now
//       d->kind = DROPKIND_DEAD; // ok to modify
//       // drops_mark(c, d->v, DROPKIND_DEAD);
//       // bitset_del(deadset2, d->id);

//       bitset_add(deadset1, d->id);
//       drop(c, d->v, loc);
//     }
//   }
// }


//—————————————————————————————————————————————————————————————————————————————————————


static irval_t* var_read_recursive(ircons_t*, irblock_t*, sym_t, type_t*, srcloc_t);


static void var_write_map(ircons_t* c, map_t* vars, sym_t name, irval_t* v) {
  irval_t** vp = (irval_t**)map_assign_ptr(vars, c->ma, name);
  if UNLIKELY(!vp)
    return out_of_mem(c);
  *vp = v;
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
  assertf(b != c->b, "defvars not yet flushed; use var_read for current block");
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
  void** vp = map_assign_ptr(phimap, c->ma, name);
  if UNLIKELY(!vp)
    return out_of_mem(c);
  assertf(*vp == NULL, "duplicate phi for %s", name);
  // if (*vp)dlog("——UNEXPECTED—— duplicate phi for %s: v%u", name, ((irval_t*)*vp)->id);
  phi->aux.ptr = b;
  *vp = phi;
}


static irval_t* var_read_recursive(
  ircons_t* c, irblock_t* b, sym_t name, type_t* type, srcloc_t loc)
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
  trace("%s b%u", __FUNCTION__, b->id);
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

  #ifdef TRACE_ANALYSIS
    trace("stash %u var%s accessed by b%u", c->vars.len, c->vars.len==1?"":"s", b->id);
    for (const mapent_t* e = map_it(&c->vars); map_itnext(&c->vars, &e); ) {
      irval_t* v = e->value;
      trace("  - %s %s = v%u", (const char*)e->key, fmtnode(0, v->type), v->id);
    }
  #endif

  // save vars
  map_t* vars = assign_block_map(c, &c->defvars, b->id);
  if UNLIKELY(vars->len != 0) {
    trace("  merge (block was reopened)");
  }
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

  if (!(b->flags & IR_FL_SEALED)) {
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


static irval_t* intconst(ircons_t* c, type_t* t, u64 value, srcloc_t loc);


static sym_t liveness_var(ircons_t* c, irval_t* v) {
  if (v->var.live != NULL)
    return v->var.live;

  char tmp[32];
  sym_t name = sym_snprintf(tmp, sizeof(tmp), ".v%u_live", v->id);
  v->var.live = name;

  srcloc_t loc = {0};
  irval_t* truev = intconst(c, type_bool, 1, loc);

  irblock_t* b = irval_block(c, v);

  // irval_t* store = pushval(c, b, OP_LOCAL, loc, type_bool);
  // pusharg(store, truev);
  // comment(c, store, name);

  var_write_inblock(c, b, name, truev);

  return name;
}


static void owners_enter_scope(ircons_t* c) {
  trace("\e[1;32m%s\e[0m", __FUNCTION__);
  if UNLIKELY(!ptrarray_push(&c->owners.entries, c->ma, (void*)(uintptr)c->owners.base))
    out_of_mem(c);
  c->owners.base = c->owners.entries.len - 1;
}

static void owners_leave_scope(ircons_t* c) {
  trace("\e[1;32m%s\e[0m", __FUNCTION__);
  c->owners.entries.len = c->owners.base;
  c->owners.base = (u32)(uintptr)c->owners.entries.v[c->owners.base];
}

static void owners_add(ircons_t* c, irval_t* v) {
  trace("\e[1;32m%s\e[0m" " v%u", __FUNCTION__, v->id);
  assert(type_isowner(v->type));
  if UNLIKELY(!ptrarray_push(&c->owners.entries, c->ma, v))
    out_of_mem(c);
}


static const char* fmtdeadset(ircons_t* c, u32 bufidx, const bitset_t* bs) {
  buf_t* buf = &c->tmpbuf[bufidx];
  buf_clear(buf);
  buf_reserve(buf, bs->cap); // approximation
  buf_push(buf, '{');
  for (usize i = 0; i < bs->cap; i++) {
    if (bitset_has(bs, i)) {
      if (buf->len > 1)
        buf_push(buf, ' ');
      buf_printf(buf, "v%zu", i);
    }
  }
  buf_push(buf, '}');
  buf_nullterm(buf);
  return buf->chars;
}


static void close_block_scope(
  ircons_t* c, u32 scope_base, irblock_t* entryb, bitset_t* entry_deadset)
{
  assertf(c->b != &bad_irblock, "no current block");

  trace("%s b%u ... b%u", __FUNCTION__, entryb->id, c->b->id);
  dlog(">> %s b%u ... b%u", __FUNCTION__, entryb->id, c->b->id);

  if (entryb != c->b) {
    for (u32 i = 0; i < entryb->values.len; i++) {
      irval_t* v = entryb->values.v[i];
      if (v->var.live) {
        dlog("b%u: read liveness var %s originating in b%u",
          c->b->id, v->var.live, entryb->id);
        // must read var to register PHI
        var_read(c, v->var.live, type_bool, (srcloc_t){0});
        // irval_t* varv = var_read(c, v->var.live, type_bool, (srcloc_t){0});
        // var_write_inblock(c, c->b, v->var.live, varv);
      }
    }
  }

  // xor computes the set difference between "dead before" and "dead after",
  // effectively "what values were killed in the scope"
  bitset_t* xor_deadset = deadset_copy(c, c->deadset);
  if UNLIKELY(!bitset_merge_xor(&xor_deadset, entry_deadset, c->ma))
    out_of_mem(c);
  // dlog("entry_deadset: (dead before)\n  %s", fmtdeadset(c, 0, entry_deadset));
  // dlog("c->deadset: (dead after)\n  %s", fmtdeadset(c, 0, c->deadset));
  // dlog("xor_deadset: (died during)\n  %s", fmtdeadset(c, 0, xor_deadset));

  // iterate over owners defined in the current scope (parent of scope that closed)
  u32 i = c->owners.entries.len;
  u32 base = c->owners.base;
  u32 entry_endid =
    entryb->values.len ? ((irval_t*)entryb->values.v[entryb->values.len-1])->id+1 : 0;
  while (i > 1) {
    i--;
    if (i == base) {
      // base = (u32)(uintptr)c->owners.entries.v[i];
      // continue;
      break;
    }
    irval_t* v = c->owners.entries.v[i];

    // if v was created in the continuation block id doesn't affect liveness diff
    if (v->id >= entry_endid)
      continue;

    if (!deadset_has(xor_deadset, v->id)) {
      dlog("  v%u is live >> drop(v%u)", v->id, v->id);
      drop(c, v, (srcloc_t){0});
      continue;
    }

    dlog("  v%u may have lost ownership", v->id);
    // TODO: detect if it _definitely_ lost ownership and just insert a drop()
    // it's likely so that when var_read(v->var.live) is not a PHI we can just drop
    // if v is dead..? Maybe?

    // if (base != c->owners.base) {
    //   dlog("    propagate to parent scope");
    //   continue;
    // }

    irval_t* control = var_read(c, v->var.live, type_bool, (srcloc_t){0});
    dlog("    control: v%u %s", control->id, op_name(control->op));
    if (control->op != OP_PHI)
      continue;

    // create "if (!.vN_live) { drop(vN) }"
    irblock_t* ifb = end_block(c);
    irval_t* end_control = ifb->control;
    irblockkind_t end_kind = ifb->kind;

    irblock_t* contb = mkblock(c, c->f, IR_BLOCK_GOTO, (srcloc_t){0});
    irblock_t* deadb = mkblock(c, c->f, IR_BLOCK_GOTO, (srcloc_t){0});

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

    drop(c, v, (srcloc_t){0});

    end_block(c);

    start_block(c, contb);
    seal_block(c, contb);
  }
  // dlog("abort");abort(); // XXX

  bitset_dispose(xor_deadset, c->ma);
}


static void move_owner(ircons_t* c, irval_t* nullable new_owner, irval_t* old_owner) {
  drops_mark(c, old_owner, DROPKIND_DEAD);
  if (new_owner) {
    drops_mark(c, new_owner, DROPKIND_LIVE);
    owners_add(c, new_owner);
    trace("\e[1;33m" "move owner: v%u -> v%u" "\e[0m", old_owner->id, new_owner->id);
  } else {
    trace("\e[1;33m" "move owner: v%u -> outside" "\e[0m", old_owner->id);
  }

  assertf(!deadset_has(c->deadset, old_owner->id), "source value is dead");
  //dlog("deadset_add v%u", old_owner->id);
  deadset_add(c, &c->deadset, old_owner->id);

  // mark as no longer live (at runtime) by setting its liveness var to false
  // TODO: only do this when inside conditionals?
  irval_t* falsev = intconst(c, type_bool, 0, (srcloc_t){0});
  sym_t old_name = liveness_var(c, old_owner);
  var_write(c, old_name, falsev);

  if (new_owner) {
    if UNLIKELY(!bitset_ensure_cap(&c->deadset, c->ma, new_owner->id+1))
      out_of_mem(c);
    assertf(!bitset_has(c->deadset, new_owner->id),
      "new_owner v%u already in deadset!", new_owner->id);
    irval_t* truev = intconst(c, type_bool, 1, (srcloc_t){0});
    sym_t new_name = liveness_var(c, new_owner);
    var_write(c, new_name, truev);
  }
}


static irval_t* move(ircons_t* c, irval_t* rvalue, srcloc_t loc) {
  if (rvalue->op == OP_PHI)
    return rvalue;
  irval_t* v = pushval(c, c->b, OP_MOVE, loc, rvalue->type);
  pusharg(v, rvalue);
  move_owner(c, v, rvalue);
  return v;
}


static irval_t* reference(ircons_t* c, irval_t* rvalue, srcloc_t loc) {
  op_t op = ((reftype_t*)rvalue->type)->ismut ? OP_BORROW_MUT : OP_BORROW;
  irval_t* v = pushval(c, c->b, op, loc, rvalue->type);
  pusharg(v, rvalue);
  return v;
}


static irval_t* move_or_copy(ircons_t* c, irval_t* rvalue, srcloc_t loc) {
  irval_t* v = rvalue;
  if (type_ismove(rvalue->type)) {
    v = move(c, rvalue, loc);
  } else if (type_isref(rvalue->type)) {
    v = reference(c, rvalue, loc);
  }
  v->var.src = rvalue->var.dst;
  return v;
}


//—————————————————————————————————————————————————————————————————————————————————————


static irval_t* expr(ircons_t* c, void* expr_node);
static irval_t* load_expr(ircons_t* c, expr_t* n);
static irval_t* load_rvalue(ircons_t* c, expr_t* origin, expr_t* n);


static irval_t* intconst(ircons_t* c, type_t* t, u64 value, srcloc_t loc) {
  // "intern" constants
  // this is a really simple solution:
  // - all constants are placed at the beginning of the entry block
  //   - first int constants, then float constants
  // - linear scan for an existing equivalent constant
  // - fast for functions with few constants, which is the common case
  // - degrades for functions with many constants
  //   - could do binary search if we bookkeep ending index
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


static irval_t* assign_local(ircons_t* c, local_t* dst, irval_t* v) {
  if (dst->name == sym__)
    return v;
  v->var.dst = dst->name;
  var_write(c, dst->name, v);
  return v;
}


static irval_t* vardef(ircons_t* c, local_t* n) {
  irval_t* v;
  if (n->init) {
    v = load_expr(c, n->init);
    v = move_or_copy(c, v, n->loc);
  } else {
    v = pushval(c, c->b, OP_ZERO, n->loc, n->type);
  }
  comment(c, v, n->name);
  return assign_local(c, n, v);
}


static irval_t* assign(ircons_t* c, binop_t* n) {
  irval_t* v = load_expr(c, n->right);
  idexpr_t* id = (idexpr_t*)n->left;
  assertf(id->kind == EXPR_ID, "TODO %s", nodekind_name(id->kind));
  v = move_or_copy(c, v, n->loc);
  assert(node_islocal(id->ref));
  comment(c, v, ((local_t*)id->ref)->name);
  return assign_local(c, (local_t*)id->ref, v);
}


static irval_t* ret(ircons_t* c, irval_t* nullable v, srcloc_t loc) {
  c->b->kind = IR_BLOCK_RET;
  if (v)
    move_owner(c, NULL, v);
  set_control(c, c->b, v);
  drops_unwind_exit(c, loc);
  return v ? v : &bad_irval;
}


static irval_t* retexpr(ircons_t* c, retexpr_t* n) {
  irval_t* v = n->value ? load_expr(c, n->value) : NULL;
  return ret(c, v, n->loc);
}


static irval_t* call(ircons_t* c, call_t* n) {
  irval_t* recv = load_expr(c, n->recv);
  dlog("TODO call args");
  irval_t* v = pushval(c, c->b, OP_CALL, n->loc, n->type);
  pusharg(v, recv);
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
          v = move_or_copy(c, v, cn->loc);
        move_owner(c, NULL, v); // move to lvalue of block (NULL b/c unknown for now)
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

  u32 scope = drops_scope_enter(c);
  owners_enter_scope(c);

  irval_t* v = blockexpr0(c, n, /*isfunbody*/false);

  owners_leave_scope(c);
  drops_unwind_scope(c, scope, n->loc);
  drops_scope_leave(c, scope);

  #ifdef CREATE_BB_FOR_BLOCK
    b = end_block(c);
    start_block(c, contb);
    seal_block(c, contb);
  #endif

  return v;
}


// static void reg_drops_since(
//   ircons_t* c, bitset_t* deadset1, const bitset_t* deadset2, u32 i, srcloc_t loc)
// {
//   // generate drops for values which lost ownership since deadset1 until deadset2
//   assertf(c->b != &bad_irblock, "no active block");
//   while (i > 0) {
//     drop_t* d = &c->drops.entries.v[--i];
//     //dlog("v%u (%s) ...", d->id, d->kind == DROPKIND_LIVE ? "live" : "dead");
//     if (bitset_has(deadset2, d->id) && !bitset_has(deadset1, d->id)) {
//       dlog(">> v%u lost ownership", d->id);
//       assert(deadset1->cap > d->id);

//       // mark as no longer live (at runtime) by setting its liveness var to false
//       sym_t name = liveness_var(c, d->v);
//       irval_t* falsev = intconst(c, type_bool, 0, (srcloc_t){0});
//       var_write(c, name, falsev);

//       // irval_t* store = pushval(c, c->b, OP_NOOP, (srcloc_t){0}, type_void);
//       // pusharg(store, d->v);
//       // commentf(c, store, "write liveness %s", name);
//     }
//   }
// }


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

  // generate control condition
  irval_t* control = load_expr(c, n->cond);
  assert(control->type == type_bool);

  // end predecessor block (leading up to and including "if")
  irblock_t* ifb = end_block(c);
  ifb->kind = IR_BLOCK_SWITCH;
  set_control(c, ifb, control);

  // create blocks for then and else branches
  irblock_t* thenb = mkblock(c, f, IR_BLOCK_GOTO, n->thenb->loc);
  irblock_t* elseb = mkblock(c, f, IR_BLOCK_GOTO, n->elseb ? n->elseb->loc : n->loc);
  u32        elseb_index = f->blocks.len - 1; // used later for moving blocks
  ifb->succs[0] = thenb;
  ifb->succs[1] = elseb; // if -> then, else
  commentf(c, thenb, "b%u.then", ifb->id);

  // copy deadset as is before entering "then" branch, in case it returns
  bitset_t* entry_deadset = deadset_copy(c, c->deadset);

  // begin "then" block
  trace("if \"then\" block");
  thenb->preds[0] = ifb; // then <- if
  start_block(c, thenb);
  seal_block(c, thenb);
  u32 scope = drops_scope_enter(c);
  owners_enter_scope(c);
  irval_t* thenv = blockexpr_noscope(c, n->thenb, /*isfunbody*/false);
  drops_unwind_scope(c, scope, n->loc);
  close_block_scope(c, scope, ifb, entry_deadset);
  owners_leave_scope(c);
  dlog("—————— end \"then\" block ——————");
  drops_scope_leave(c, scope);
  u32 thenb_nvars = c->vars.len; // save number of vars modified by the "then" block

  // if "then" block returns, undo deadset changes made by the "then" block
  if (c->b->kind == IR_BLOCK_RET) {
    trace("\"then\" block returns -- undo deadset changes from \"then\" block");
    if UNLIKELY(!bitset_copy(&c->deadset, entry_deadset, c->ma))
      out_of_mem(c);
  }

  // end & seal "then" block
  thenb = end_block(c);

  irval_t* elsev;

  // begin "else" branch (if there is one)
  if (n->elseb) {
    trace("if \"else\" block");

    // copy deadset as is before entering "else" branch, in case it returns
    bitset_t* then_entry_deadset = deadset_copy(c, c->deadset);

    // undo kills made in "then" branch
    // FIXME: duplicate (also done earlier when thenb->kind==IR_BLOCK_RET)
    if UNLIKELY(!bitset_copy(&c->deadset, entry_deadset, c->ma))
      out_of_mem(c);

    // begin "else" block
    commentf(c, elseb, "b%u.else", ifb->id);
    elseb->preds[0] = ifb; // else <- if
    start_block(c, elseb);
    seal_block(c, elseb);
    u32 scope = drops_scope_enter(c);
    owners_enter_scope(c);
    elsev = blockexpr_noscope(c, n->elseb, /*isfunbody*/false);
    drops_unwind_scope(c, scope, n->loc);
    close_block_scope(c, scope, ifb, entry_deadset);
    owners_leave_scope(c);
    drops_scope_leave(c, scope);

    // if "then" block returns, no "cont" block needed
    // e.g. "fun f() { if true { 1 } else { return 2 }; return 3 }"
    if (thenb->kind == IR_BLOCK_RET) {
      // discard_block(c, contb);
      bitset_dispose(entry_deadset, c->ma);
      bitset_dispose(then_entry_deadset, c->ma);
      return elsev;
    }

    // if (elseb->kind != IR_BLOCK_RET) {
    //   // check if "then" branch caused loss of ownership of outer values
    //   u32 then_drops_i = drops_since_lastindex(c, entry_deadset, then_entry_deadset);
    //   if (then_drops_i) {
    //     trace("gen synthetic drops in \"else\" branch b%u", c->b->id);
    //     // generate drops in "else" branch for outer values dropped in "then" branch
    //     reg_drops_since(c, entry_deadset, then_entry_deadset, then_drops_i, n->loc);
    //     //drops_since_gen(c, entry_deadset, then_entry_deadset, then_drops_i, n->loc);
    //   }
    // }

    u32 elseb_nvars = c->vars.len; // save number of vars modified by the "else" block
    elseb = end_block(c);

    // if "else" block returns, undo deadset changes made by the "else" block
    if (elseb->kind == IR_BLOCK_RET) {
      trace("\"else\" block returns -- undo deadset changes from \"else\" block");
      if UNLIKELY(!bitset_copy(&c->deadset, then_entry_deadset, c->ma))
        out_of_mem(c);
    } else {
      // // check if "else" branch caused loss of ownership of outer values
      // u32 else_drops_i = drops_since_lastindex(c, then_entry_deadset, c->deadset);
      // if (else_drops_i) {
      //   // generate drops in "then" branch for outer values dropped in "else" branch
      //   trace("gen synthetic drops in \"then\" branch b%u", thenb->id);
      //   start_block(c, thenb);
      //   reg_drops_since(c, then_entry_deadset, c->deadset, else_drops_i, n->loc);
      //   // drops_since_gen(c, then_entry_deadset, c->deadset, else_drops_i, n->loc);
      //   end_block(c);
      // }
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
      ifb->succs[0] = contb;   // if true —> cont
      contb->preds[0] = ifb;   // cont[0] <— if
      contb->preds[1] = elseb; // cont[1] <— else
      discard_block(c, thenb); // trash thenb
      thenb = contb;
    } else if (elseb_isnoop) {
      // "then" branch has no effect; cut it out
      trace("eliding \"else\" branch");
      thenb->succs[0] = contb; // then —> cont
      ifb->succs[1] = contb;   // if false —> cont
      contb->preds[0] = thenb; // cont[0] <— then
      contb->preds[1] = ifb;   // cont[1] <— if
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
        contb->preds[0] = thenb; // cont[0] <— then
        contb->preds[1] = elseb; // cont[1] <— else
      }
    }

    // begin continuation block
    start_block(c, contb);
    seal_block(c, contb);
  } else {
    // no "else" branch

    // check if "then" branch caused loss of ownership of outer values
    u32 then_drops_i = 0;
    if (thenb->kind != IR_BLOCK_RET)
      then_drops_i = drops_since_lastindex(c, entry_deadset, c->deadset);

    if (then_drops_i) {
      // begin "else" branch
      commentf(c, elseb, "b%u.else", ifb->id);
      elseb->preds[0] = ifb; // else <- if
      start_block(c, elseb);
      seal_block(c, elseb);

      // generate drops for values dropped in "then" branch
      //drops_since_gen(c, entry_deadset, c->deadset, then_drops_i, n->loc);

      // end "else" branch
      elseb = end_block(c);

      // create continuation block (the block after the "if")
      irblock_t* contb = mkblock(c, f, IR_BLOCK_GOTO, n->loc);
      commentf(c, contb, "b%u.cont", ifb->id);

      // wire up graph edges
      elseb->succs[0] = contb; // else —> cont
      thenb->succs[0] = contb; // then —> cont
      contb->preds[0] = thenb; // cont[0] <— then
      contb->preds[1] = elseb; // cont[1] <— else

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
  assert_types_iscompat(c, left->type, right->type);
  irval_t* v = pushval(c, c->b, n->op, n->loc, n->type);
  pusharg(v, left);
  pusharg(v, right);
  return v;
}


static irval_t* intlit(ircons_t* c, intlit_t* n) {
  return intconst(c, n->type, n->intval, n->loc);
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


static void check_borrowing(ircons_t* c, irfun_t* f) {
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

  // just a declaration?
  if (n->body == NULL)
    return f;

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
  irblock_t* entryb = mkblock(c, f, IR_BLOCK_GOTO, n->loc);
  start_block(c, entryb);
  seal_block(c, entryb); // entry block has no predecessors

  // enter function scope
  u32 scope = drops_scope_enter(c);
  owners_enter_scope(c);

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
      owners_add(c, v);
      drops_mark(c, v, DROPKIND_LIVE);
    }

    var_write(c, param->name, v);
  }

  // check if function has implicit return value
  if (((funtype_t*)n->type)->result != type_void && n->body->children.len) {
    expr_t* lastexpr = n->body->children.v[n->body->children.len-1];
    if (lastexpr->kind != EXPR_RETURN)
      n->body->flags |= EX_RVALUE;
  }

  bitset_t* entry_deadset = deadset_copy(c, c->deadset);

  // build body
  irval_t* body = blockexpr_noscope(c, n->body, /*isfunbody*/true);

  // reset EX_RVALUE flag, in case we set it above
  n->body->flags &= ~EX_RVALUE;

  // handle implicit return
  // note: if the block ended with a "return" statement, b->kind is already BLOCK_RET
  if (c->b->kind != IR_BLOCK_RET)
    ret(c, body != &bad_irval ? body : NULL, n->body->loc);

  // end final block of the function
  close_block_scope(c, scope, entryb, entry_deadset);
  bitset_dispose(entry_deadset, c->ma);
  end_block(c);

  // leave function scope
  drops_scope_leave(c, scope);
  owners_leave_scope(c);

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

  check_borrowing(c, f);

end:
  c->f = &bad_irfun;
  return f;
}


static irval_t* funexpr(ircons_t* c, fun_t* n) {
  irfun_t* f = fun(c, n);
  irval_t* v = pushval(c, c->b, OP_FUN, n->loc, n->type);
  v->aux.ptr = f;
  if (n->name)
    comment(c, v, n->name);
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
  if (!type_isowner(n->type))
    return v;

  if (bitset_has(c->deadset, v->id))
    goto err_dead;

  // drop_t* d = drops_lookup(c, v->id);
  // if (!d) {
  //   error(c, origin, "use of uninitialized %s %s", nodekind_fmt(n->kind), n->name);
  // } else if (d->kind != DROPKIND_LIVE) {
  //   goto err_dead;
  // }
  return v;

err_dead:
  error(c, origin, "use of dead value %s", n->name);
  srcrange_t moved_srcrange = find_moved_srcrange(c, v->id);
  if (moved_srcrange.focus.line)
    help(c, moved_srcrange, "%s moved here", n->name);
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
  if (n->kind == EXPR_ID)
    return load_rvalue(c, n, asexpr(((idexpr_t*)n)->ref));
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
  case EXPR_DEREF:     return deref(c, n, (unaryop_t*)n);
  case EXPR_FLOATLIT:  return floatlit(c, (floatlit_t*)n);
  case EXPR_ID:        return idexpr(c, (idexpr_t*)n);
  case EXPR_FUN:       return funexpr(c, (fun_t*)n);
  case EXPR_IF:        return ifexpr(c, (ifexpr_t*)n);
  case EXPR_RETURN:    return retexpr(c, (retexpr_t*)n);

  case EXPR_BOOLLIT:
  case EXPR_INTLIT:
    return intlit(c, (intlit_t*)n);

  case EXPR_VAR:
  case EXPR_LET:
    return vardef(c, (local_t*)n);

  // TODO
  case EXPR_MEMBER:    // return member(c, (member_t*)n);
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

  for (u32 i = 0; i < n->children.len && c->compiler->errcount == 0; i++) {
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

  c.deadset = assertnotnull( bitset_make(c.ma, BITSET_STACK_CAP) );

  if (!map_init(&c.funm, c.ma, MAX(n->children.len,1)*2)) {
    c.err = ErrNoMem;
    goto fail_funm;
  }

  if (!map_init(&c.vars, c.ma, 8)) {
    c.err = ErrNoMem;
    goto fail_vars;
  }

  for (usize i = 0; i < countof(c.tmpbuf); i++)
    buf_init(&c.tmpbuf[i], c.ma);

  irunit_t* u = unit(&c, n);

  // end
  *result = (u == &bad_irunit) ? NULL : u;

  for (usize i = 0; i < countof(c.tmpbuf); i++)
    buf_dispose(&c.tmpbuf[i]);

  droparray_dispose(&c.drops.entries, c.ma);
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


err_t analyze2(compiler_t* compiler, memalloc_t ir_ma, unit_t* unit) {
  irunit_t* u;
  err_t err = ircons(compiler, ir_ma, unit, &u);
  assertnotnull(u);
  dump_irunit(u, compiler->ma);
  debug_graphviz(u, compiler->ma);
  return ErrCanceled; // XXX
  return err;
}
