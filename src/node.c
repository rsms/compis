// diagnostics reporting
#include "c0lib.h"
#include "compiler.h"
#include "abuf.h"


typedef enum reprflag {
  REPRFLAG_HEAD = 1 << 0, // is list head
} reprflag_t;


// node kind string table with compressed indices (compared to table of pointers.)
// We end up with something like this; one string with indices:
//   enum {
//     NK_NBAD, NK__NBAD = NK_NBAD + strlen("NBAD"),
//     NK_NCOMMENT, NK__NCOMMENT = NK_NCOMMENT + strlen("NCOMMENT"),
//     NK_NUNIT, NK__NUNIT = NK_NUNIT + strlen("NUNIT"),
//     NK_NFUN, NK__NFUN = NK_NFUN + strlen("NFUN"),
//     NK_NBLOCK, NK__NBLOCK = NK_NBLOCK + strlen("NBLOCK"),
//     NK_UNKNOWN, NK__UNKNOWN = NK_UNKNOWN + strlen("N?"),
//   };
//   static const struct { u8 offs[NODEKIND_COUNT]; char strs[]; } nktab = {
//     { NK_NBAD, NK_NCOMMENT, NK_NUNIT, NK_NFUN, NK_NBLOCK },
//     { "NBAD\0NCOMMENT\0NUNIT\0NFUN\0NBLOCK\0N?" }
//   };
#define NK_UNKNOWN_STR "N?"
enum {
  #define _(NAME) NK_##NAME, NK__##NAME = NK_##NAME + strlen(#NAME),
  FOREACH_NODE_KIND(_)
  #undef _
  NK_UNKNOWN, NK__UNKNOWN = NK_UNKNOWN + strlen(NK_UNKNOWN_STR),
};
static const struct {
  u8   offs[NODEKIND_COUNT + 1]; // index into strs
  char strs[];
} nktab = {
  { // get offset from enum
    #define _(NAME) NK_##NAME,
    FOREACH_NODE_KIND(_)
    #undef _
    NK_UNKNOWN,
  }, {
    #define _(NAME) #NAME "\0"
    FOREACH_NODE_KIND(_)
    #undef _
    NK_UNKNOWN_STR
  }
};


static void repr_kind(abuf_t* s, const node_t* n) {
  u8 start = nktab.offs[MIN(n->kind, NODEKIND_COUNT)];
  const char* p = &nktab.strs[start] + 1;
  abuf_str(s, p);
}


static void repr(abuf_t* s, const node_t* n, usize indent, reprflag_t fl) {
  if ((fl & REPRFLAG_HEAD) == 0) {
    abuf_c(s, '\n');
    abuf_fill(s, ' ', indent);
  }
  fl &= ~REPRFLAG_HEAD;
  indent += 2;

  if (n->kind == NBAD)
    return abuf_str(s, "BAD");

  abuf_c(s, '(');
  repr_kind(s, n);

  switch (n->kind) {
  case NINTLIT:
    abuf_c(s, ' '); abuf_u64(s, n->intval, 10); break;
  case NID:
    abuf_c(s, ' '); abuf_str(s, n->strval); break;
  default:
    break;
  }

  for (node_t* cn = n->children.head; cn; cn = cn->next) {
    abuf_c(s, ' ');
    repr(s, cn, indent, fl);
  }

  abuf_c(s, ')');
}

err_t node_repr(buf_t* buf, const node_t* n) {
  usize needavail = 4096;
  for (;;) {
    buf_reserve(buf, needavail);
    abuf_t s = abuf_make(buf->p, buf->cap);
    repr(&s, n, 0, REPRFLAG_HEAD);
    usize len = abuf_terminate(&s);
    if (len < needavail) {
      buf->len += len;
      break;
    }
    needavail = len + 1;
  }
  return 0;
}


void node_free(memalloc_t ast_ma, node_t* n) {
  if (n == &last_resort_node)
    return;
  for (node_t* cn = n->children.head; cn;) {
    node_t* cn2 = cn->next;
    node_free(ast_ma, cn);
    cn = cn2;
  }
  if (node_has_strval(n)) {
    mem_t m = MEM(n->strval, strlen(n->strval));
    mem_free(ast_ma, &m);
  }
  mem_freet(ast_ma, n);
}
