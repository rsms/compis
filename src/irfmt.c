// SPDX-License-Identifier: Apache-2.0
#include "colib.h"
#include "ir.h"
#include "compiler.h"


typedef struct {
  compiler_t*  c;
  const pkg_t* pkg;
  buf_t        out;
  bool         ok;
} fmtctx_t;


#define MEMFAIL()            (ctx->ok = false, ((void)0))
#define CHAR(ch)             buf_push(&ctx->out, (ch))
#define PRINT(cstr)          buf_print(&ctx->out, (cstr))
#define PRINTF(fmt, args...) buf_printf(&ctx->out, (fmt), ##args)
#define FILL(byte, len)      buf_fill(&ctx->out, (byte), (len))
#define TABULATE(start, dstcol)   \
  FILL(' ', (usize)(MAX(1, (int)(dstcol) - (int)(ctx->out.len - (start)))));

#define COMMENT_COL  32


static bool ismemonly(const irval_t* v) {
  return v->type == type_void;
}

static bool has_side_effects(const irval_t* v) {
  return v->op == OP_MOVE;
}


static void val(fmtctx_t* ctx, const irval_t* v, bool isdot) {
  u32 start = ctx->out.len + 1;
  PRINTF(isdot ? "\n" : "\n    ");

  // showtype: enable to include syntax-formatted type after name
  // e.g. "v1 int = ..." instead of "v1 = ..."
  const bool showtype = true;

  if (!ismemonly(v)) {
    if (isdot || !showtype) {
      PRINTF("v%-2u = ", v->id);
    } else {
      PRINTF("v%-2u ", v->id);
      u32 tstart = ctx->out.len;
      node_fmt(&ctx->out, (node_t*)v->type, 0);
      PRINTF("%*s = ", MAX(0, 4 - (int)(ctx->out.len - tstart)), "");
    }
  }

  PRINTF("%-*s", 6, op_name(v->op) + 3/*"OP_"*/);

  for (u32 i = 0; i < v->argc; i++)
    PRINTF(" v%-2u", v->argv[i]->id);

  switch (v->op) {
  case OP_ARG:
    PRINTF(" %u", v->aux.i32val);
    break;
  case OP_ICONST:
    PRINTF(" 0x%llx", v->aux.i64val);
    break;
  case OP_GEP:
    PRINTF(" %llu", v->aux.i64val);
    break;
  case OP_FCONST:
    PRINTF(" %g", v->aux.f64val);
    break;
  case OP_DROP:
    if (isdot && v->var.src)
      PRINTF(" (%s)", v->var.src);
    break;
  case OP_FUN: {
    const irfun_t* f = assertnotnull(v->aux.ptr);
    PRINTF(" %s", f->name);
    break;
  }
  case OP_STR:
    PRINT(" \"");
    usize maxlen = 15;
    buf_appendrepr(&ctx->out, v->aux.bytes.p, MIN(v->aux.bytes.len, maxlen));
    CHAR('\"');
    if (v->aux.bytes.len > maxlen)
      PRINTF("+%zu", v->aux.bytes.len - maxlen);
    break;
  }

  if (isdot)
    return;

  TABULATE(start, COMMENT_COL);
  if (ismemonly(v)) {
    PRINT("# [M]");
  } else {
    PRINTF("# [%u]", v->nuse);
  }
  if (v->comment && *v->comment)
    CHAR(' '), PRINT(v->comment);

  if (v->var.src || v->var.dst) {
    PRINT(" {");
    if (v->var.dst) {
      PRINTF("dst=%s", v->var.dst);
      if (v->var.src)
        PRINTF(" src=%s", v->var.src);
    } else {
      PRINTF("src=%s", v->var.src);
    }
    PRINT("}");
  }

  if (loc_line(v->loc)) {
    TABULATE(start, COMMENT_COL + 25);
    const srcfile_t* sf = loc_srcfile(v->loc, &ctx->c->locmap);
    if (sf) {
      PRINTF(" %s:%u:%u", sf->name.p, loc_line(v->loc), loc_col(v->loc));
    } else {
      PRINTF(" %u:%u", loc_line(v->loc), loc_col(v->loc));
    }
  }
}


static void block(fmtctx_t* ctx, const irblock_t* b) {
  u32 start = ctx->out.len + 1;
  PRINTF("\n    b%u:", b->id);

  if (b->preds[1]) {
    PRINTF(" <- b%u b%u", assertnotnull(b->preds[0])->id, b->preds[1]->id);
  } else if (b->preds[0]) {
    PRINTF(" <- b%u", b->preds[0]->id);
  }

  if (b->comment) {
    TABULATE(start, COMMENT_COL);
    PRINTF("# %s", b->comment);
  }

  for (u32 i = 0; i < b->values.len; i++)
    val(ctx, b->values.v[i], /*isdot*/false);

  switch (b->kind) {
    case IR_BLOCK_GOTO: {
      if (b->succs[0]) {
        PRINTF("\n    goto -> b%u", b->succs[0]->id);
      } else {
        PRINT("\n    goto -> ?");
      }
      break;
    }
    case IR_BLOCK_SWITCH: {
      assert(b->succs[0]); // thenb
      assert(b->succs[1]); // elseb
      assertf(b->control, "missing control value");
      PRINTF("\n    switch v%u -> b%u b%u",
        b->control->id, b->succs[0]->id, b->succs[1]->id);
      break;
    }
    case IR_BLOCK_RET: {
      if (b->control) {
        PRINTF("\n    ret v%u", b->control->id);
      } else {
        PRINT("\n    ret");
      }
      break;
    }
  }
}


static void fun(fmtctx_t* ctx, const irfun_t* f) {
  if (ctx->out.len)
    CHAR('\n');

  PRINT("  fun ");

  if (f->ast) {
    compiler_fully_qualified_name(ctx->c, ctx->pkg, &ctx->out, (node_t*)f->ast);
    CHAR('(');
    funtype_t* ft = (funtype_t*)f->ast->type;
    for (u32 i = 0; i < ft->params.len; i++) {
      const local_t* param = (local_t*)ft->params.v[i];
      if (i) PRINT(", ");
      node_fmt(&ctx->out, (node_t*)param, 0);
    }
    PRINT(") ");
    node_fmt(&ctx->out, (node_t*)ft->result, 0);
  } else {
    // best effort; name will be wrong for type functions (missing recvt)
    PRINTF("%s.%s(", ctx->pkg->path.p, f->name);
    if (f->blocks.len) {
      const irblock_t* b = f->blocks.v[0];
      for (u32 i = 0, n = 0; i < b->values.len; i++) {
        const irval_t* v = b->values.v[i];
        if (v->op == OP_ARG) {
          if (n++) PRINT(", ");
          node_fmt(&ctx->out, (node_t*)v->type, 0);
        }
      }
    }
    PRINT(") ");
    const type_t* restype = type_void;
    for (u32 i = 0; i < f->blocks.len; i++) {
      const irblock_t* b = f->blocks.v[i];
      if (b->kind == IR_BLOCK_RET) {
        if (b->control)
          restype = b->control->type;
        break;
      }
    }
    node_fmt(&ctx->out, (node_t*)restype, 0);
  }

  if (f->blocks.len > 0) {
    PRINT(" {");
    for (u32 i = 0; i < f->blocks.len; i++)
      block(ctx, f->blocks.v[i]);
    PRINT("\n  }");
  }
}


static void block_dot_nodes(fmtctx_t* ctx, const char* ns, const irblock_t* b) {
  const char* b_bgcolor = "#cccccc";
  if (b->id == 0) {
    b_bgcolor = "#55ff88";
  } else if (b->kind == IR_BLOCK_RET) {
    b_bgcolor = "#ff9988";
  } else if (b->kind == IR_BLOCK_SWITCH) {
    b_bgcolor = "#77ccff";
  }

  PRINTF("  \"%s.b%u\" [shape=\"none\", label=<<table border=\"0\" cellborder=\"1\" cellspacing=\"0\"><tr><td bgcolor=\"%s\" align=\"center\" colspan=\"1\"><font color=\"black\">%u</font></td></tr>",
    ns, b->id, b_bgcolor, b->id);

  if (b->values.len) {
    PRINT("<tr><td align=\"left\" balign=\"left\">");
    for (u32 i = 0; i < b->values.len; i++) {
      const irval_t* v = b->values.v[i];
      bool dimmed = !ismemonly(v) && v->nuse == 0 && !has_side_effects(v);
      if (dimmed)
        PRINT("<font color=\"#ffffff99\">");
      val(ctx, v, /*isdot*/true);
      if (dimmed)
        PRINT("</font>");
      PRINT("<br/>");
    }
    PRINT("</td></tr>");
  }

  PRINT("<tr><td align=\"left\">");
  switch (b->kind) {
    case IR_BLOCK_GOTO:
      PRINT("goto");
      break;
    case IR_BLOCK_SWITCH:
      PRINTF("switch v%u", assertnotnull(b->control)->id);
      break;
    case IR_BLOCK_RET:
      if (b->control) {
        PRINTF("ret v%u", b->control->id);
      } else {
        PRINTF("ret");
      }
      break;
  }
  PRINT("</td></tr>");

  PRINT("</table>>]");
}


static void block_dot_edges(fmtctx_t* ctx, const char* ns, const irblock_t* b) {
  switch (b->kind) {
    case IR_BLOCK_GOTO: {
      assertf(b->succs[0], "b%u", b->id);
      PRINTF("  \"%s.b%u\" -> \"%s.b%u\";\n", ns, b->id, ns, b->succs[0]->id);
      break;
    }
    case IR_BLOCK_SWITCH: {
      assertnotnull(b->succs[0]);
      assertnotnull(b->succs[1]);
      PRINTF("  \"%s.b%u\" -> \"%s.b%u\" [label=\" 0 \"];\n",
        ns, b->id, ns, b->succs[0]->id);
      PRINTF("  \"%s.b%u\" -> \"%s.b%u\" [label=\" 1 \"];\n",
        ns, b->id, ns, b->succs[1]->id);
      break;
    }
    // case IR_BLOCK_RET: {
    //   // PRINTF("  \"%s.exit\" [style=invis];\n", ns);
    //   PRINTF("  \"%s.exit\" [label=end, shape=plain];\n", ns);
    //   PRINTF("  \"%s.b%u\" -> \"%sexit\" [label=\"return\"];\n", ns, b->id, ns);
    //   break;
    // }
  }
}


static void fun_dot(fmtctx_t* ctx, const irfun_t* f) {
  buf_t namebuf = buf_make(ctx->out.ma);
  compiler_fully_qualified_name(ctx->c, ctx->pkg, &namebuf, (node_t*)f->ast);
  buf_nullterm(&namebuf);
  const char* name = namebuf.chars;
  if (f->blocks.len == 0) {
    // declaration only, no body
    PRINTF("  \"%s.b0\" [label=\"decl-only\";shape=none];\n", name);
  } else {
    for (u32 i = 0; i < f->blocks.len; i++)
      block_dot_nodes(ctx, name, f->blocks.v[i]);
    for (u32 i = 0; i < f->blocks.len; i++)
      block_dot_edges(ctx, name, f->blocks.v[i]);
  }
  buf_dispose(&namebuf);
}


static void unit(fmtctx_t* ctx, const irunit_t* u) {
  if (ctx->out.len)
    CHAR('\n');
  if (u->srcfile) {
    PRINTF("unit \"%s\" {", u->srcfile->name.p);
  } else {
    PRINT("unit {");
  }

  for (u32 i = 0; i < u->functions.len; i++) {
    const irfun_t* f = u->functions.v[i];
    if (f->blocks.len > 0)
      fun(ctx, f);
  }

  if (u->functions.len)
    CHAR('\n');
  CHAR('}');
}


static fmtctx_t fmtctx_make(compiler_t* c, const pkg_t* pkg, buf_t* out) {
  return (fmtctx_t){
    .c = c,
    .ok = true,
    .out = *out,
    .pkg = pkg,
  };
}


bool irfmt(compiler_t* c, const pkg_t* pkg, buf_t* out, const irunit_t* u) {
  fmtctx_t ctx = fmtctx_make(c, pkg, out);
  unit(&ctx, u);
  *out = ctx.out;
  return ctx.ok && buf_nullterm(out);
}


bool irfmt_fun(compiler_t* c, const pkg_t* pkg, buf_t* out, const irfun_t* f) {
  fmtctx_t ctx = fmtctx_make(c, pkg, out);
  fun(&ctx, f);
  *out = ctx.out;
  return ctx.ok && buf_nullterm(out);
}


bool irfmt_dot(compiler_t* c, const pkg_t* pkg, buf_t* out, const irunit_t* u) {
  fmtctx_t ctx = fmtctx_make(c, pkg, out);

  #define DOT_FONT "fontname=\"JetBrains Mono NL, Menlo, Courier, monospace\";"
  buf_print(&ctx.out,
    "digraph G {\n"
    "  overlap=false;\n"
    "  pad=0.2;\n"
    "  margin=0;\n"
    "  bgcolor=\"#1A1A19\";\n"
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
      buf_printf(&ctx.out,
        "subgraph cluster%p {\n"
        "penwidth=1; color=\"#ffffff77\"; margin=4;\n"
        "fontcolor=\"#ffffff77\"; " DOT_FONT " fontsize=14;\n"
        "labeljust=l; label=\""
        , f);
      compiler_fully_qualified_name(ctx.c, pkg, &ctx.out, (node_t*)f->ast);
      buf_print(&ctx.out, "\"\n");
    }
    fun_dot(&ctx, f);
    if (nfuns > 1)
      buf_print(&ctx.out, "}\n");
  }

  buf_push(&ctx.out, '}');

  *out = ctx.out;
  return ctx.ok && buf_nullterm(out);
}
