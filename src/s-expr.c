#include "colib.h"
#include "s-expr.h"

typedef struct s_expr_parser_t {
  memalloc_t           ma;    // memory allocator for s_expr_t nodes
  err_t                err;
  s_expr_diag_handler_t diag_handler;
  const u8*            start; // start of source
  const u8*            end;   // end of source
  const u8*            curr;  // current pointer in source
  const u8*            linestart;
  u32                  line;
  void* nullable       userdata;
} s_expr_parser_t;


static void s_expr_parse_atom(s_expr_parser_t* p, s_expr_atom_t* atom) {
  atom->chars = (const char*)p->curr;
  while (p->curr < p->end) {
    u8 b = *p->curr;
    switch (b) {
      case ' ': case '\t':
      case '\r': case '\n':
      case '(': case ')':
      case '[': case ']':
      case '{': case '}':
        goto end;
      default:
        break;
    }
    p->curr++;
  }
end:
  atom->len = (uintptr_t)((const char*)p->curr - atom->chars);
}


inline static u8 s_expr_endtok(u8 starttok) {
  // ASCII/UTF8: ( ) ... [ \ ] ... { | }
  return starttok == '(' ? starttok + 1 : starttok + 2;
}


static void s_expr_skip_line(s_expr_parser_t* p) {
  // note: expects p->curr to be at some byte that triggered the "skip", like ';'
  while (p->curr < p->end) {
    if (*(++p->curr) == '\n') {
      p->line++;
      p->linestart = p->curr;
      break;
    }
  }
}


static void s_expr_default_diag_handler(
  const s_expr_diag_t* diag, void* nullable userdata)
{
  elog("s-expr:%u:%u: %s", diag->line, diag->col, diag->message);
}


static void s_expr_diag_err(s_expr_parser_t* p, const char* fmt, ...) {
  if (p->err == 0)
    p->err = ErrInvalid;

  buf_t buf = buf_make(p->ma);

  va_list ap;
  va_start(ap, fmt);
  buf_vprintf(&buf, fmt, ap);
  va_end(ap);

  if UNLIKELY(!buf_nullterm(&buf)) {
    elog("nomem");
  } else {
    p->diag_handler(&(s_expr_diag_t){
      .line    = p->line + 1,
      .col     = (uintptr)(p->curr - p->linestart) + 1,
      .message = buf.chars,
    }, p->userdata);
  }

  buf_dispose(&buf);
}


static bool s_expr_parse_list(s_expr_parser_t* p, u8 endtok, s_expr_list_t* list) {
  s_expr_t* tail = NULL;
  while (p->curr < p->end) {
    s_expr_t* n = NULL;

    u8 b = *p->curr++;
    switch (b) {
      case ' ': case '\t':
      case '\r':
        // skip whitespace
        break;
      case '\n':
        p->line++;
        p->linestart = p->curr;
        break;
      case ';': // ";" line comment
        s_expr_skip_line(p);
        break;
      case '(': case '[': case '{': {
        s_expr_list_t* l = mem_alloct(p->ma, s_expr_list_t);
        if (!l)
          return false;
        l->type = SEXPR_LIST;
        l->kind = b;
        if (!s_expr_parse_list(p, s_expr_endtok(b), l))
          return false;
        n = (s_expr_t*)l;
        break;
      }
      case ')': case ']': case '}':
        if UNLIKELY(b != endtok) {
          if (endtok == 0) {
            // top-level
            s_expr_diag_err(p, "extraneous '%C'", b);
          } else {
            s_expr_diag_err(p, "unexpected '%C'; expected '%C'", b, endtok);
          }
          return false;
        }
        return true;
      default: {
        s_expr_atom_t* a = mem_alloct(p->ma, s_expr_atom_t);
        if (!a)
          return false;
        a->type = SEXPR_ATOM;
        p->curr--;
        s_expr_parse_atom(p, a);
        n = (s_expr_t*)a;
        break;
      }
    }

    if (n) {
      if (tail) {
        tail->next = n;
      } else {
        list->head = n;
      }
      tail = n;
    }
  }

  if UNLIKELY(endtok != 0)
    s_expr_diag_err(p, "unterminated list, missing closing '%C'", endtok);

  return false;
}


err_t s_expr_parse(
  s_expr_list_t**                result_out,
  slice_t                       src,
  memalloc_t                    ma,
  s_expr_diag_handler_t nullable diag_handler,
  void* nullable                userdata)
{
  s_expr_parser_t p = {
    .ma = ma,
    .diag_handler = diag_handler ? diag_handler : s_expr_default_diag_handler,
    .start = src.bytes,
    .end = src.bytes + src.len,
    .curr = src.bytes,
    .linestart = src.bytes,
    .userdata = userdata,
  };

  s_expr_list_t* list = mem_alloct(p.ma, s_expr_list_t);
  list->type = SEXPR_LIST;
  list->kind = '.';
  s_expr_parse_list(&p, 0, list);

  if UNLIKELY(p.err) {
    mem_freet(ma, list);
    return p.err;
  }

  if (list->head && list->head->type == SEXPR_LIST && list->head->next == NULL) {
    // input is single explicit list, e.g. "(a b c)"
    *result_out = (s_expr_list_t*)list->head;
    mem_freet(ma, list);
  } else {
    *result_out = list;
  }

  return 0;
}


static void s_expr_fmt_compact(buf_t* buf, const s_expr_t* n) {
  if (n->type == SEXPR_ATOM) {
    const s_expr_atom_t* a = (s_expr_atom_t*)n;
    buf_append(buf, a->chars, a->len);
  } else {
    const s_expr_list_t* l = (s_expr_list_t*)n;
    if (l->kind != '.')
      buf_push(buf, l->kind);
    for (const s_expr_t* cn = l->head; cn;) {
      s_expr_fmt_compact(buf, cn);
      if (!(cn = cn->next))
        break;
      buf_push(buf, ' ');
    }
    if (l->kind != '.')
      buf_push(buf, s_expr_endtok(l->kind));
  }
}


static void s_expr_fmt_pretty(
  buf_t*          buf,
  u32             maxcol,
  usize           bufstart,
  const s_expr_t* n,
  usize           linestart,
  int             depth)
{
  if (n->type == SEXPR_ATOM) {
    const s_expr_atom_t* a = (s_expr_atom_t*)n;
    buf_append(buf, a->chars, a->len);
    return;
  }

  // SEXPR_LIST
  s_expr_list_t* l = (s_expr_list_t*)n;
  if (l->kind != '.') {
    buf_push(buf, l->kind);
    depth++;
  }

  usize bufstart_list = buf->len;
  usize bufstart_last = buf->len;
  bool insert_linebreaks = l->head && l->head->type == SEXPR_LIST;
  const s_expr_t* first_list_cn = NULL;

  for (const s_expr_t* cn = l->head; cn; ) {
    if (cn->type == SEXPR_LIST && !first_list_cn) {
      first_list_cn = cn;
      bufstart_list = buf->len;
    }

    if (insert_linebreaks) {
      if (buf->len == bufstart && l->kind == '.') {
        // special case for first item of implicit root list
        linestart = buf->len;
      } else {
        linestart = (u32)buf->len + 1;
        buf_push(buf, '\n');
        buf_fill(buf, ' ', (usize)depth * 2);
      }
    } else {
      bufstart_last = buf->len;
      if (cn != l->head)
        buf_push(buf, ' ');
    }

    s_expr_fmt_pretty(buf, maxcol, bufstart, cn, linestart, depth);

    if (buf->len - linestart > maxcol) {
      if (!insert_linebreaks) {
        if (first_list_cn) {
          // rewind to first list entry
          cn = first_list_cn;
          buf->len = bufstart_list;
        } else {
          // undo this one entry
          buf->len = bufstart_last;
        }
        insert_linebreaks = true;
        continue;
      }
    }

    if (!cn->next)
      break;
    cn = cn->next;
  }

  if (l->kind != '.')
    buf_push(buf, s_expr_endtok(l->kind));
}


err_t _s_expr_fmt(const s_expr_t* n, buf_t* buf, u32 flags, u32 maxcol) {
  if (flags & SEXPR_FMT_PRETTY) {
    s_expr_fmt_pretty(buf, maxcol, buf->len, n, buf->len, 0);
  } else {
    s_expr_fmt_compact(buf, n);
  }
  return buf_nullterm(buf) ? 0 : ErrNoMem;
}


err_t s_expr_prettyprint(buf_t* dst, slice_t src, u32 maxcol) {
  memalloc_t ma = memalloc_ctx();
  s_expr_list_t* list;
  err_t err = s_expr_parse(&list, src, ma, NULL, NULL);
  if (err)
    return err;
  buf_reserve(dst, src.len);
  err = s_expr_fmt(list, dst, SEXPR_FMT_PRETTY, maxcol);
  s_expr_free(list, ma);
  return err;
}


static void s_expr_free1(memalloc_t ma, s_expr_t* n) {
  if (n->type == SEXPR_LIST) {
    s_expr_t* cn = ((s_expr_list_t*)n)->head;
    while (cn) {
      s_expr_t* next_cn = cn->next;
      s_expr_free1(ma, cn);
      cn = next_cn;
    }
  }
  mem_freet(ma, n);
}

void s_expr_free(s_expr_list_t* n, memalloc_t ma) {
  s_expr_free1(ma, (s_expr_t*)n);
}


s_expr_t* nullable s_expr_at(s_expr_list_t* list, usize index) {
  SEXPR_LIST_FOREACH(list, n) {
    if (index == 0)
      return n;
    index--;
  }
  return NULL;
}


s_expr_atom_t* s_expr_atom_at(s_expr_list_t* list, usize index) {
  s_expr_t* n = s_expr_at(list, index);
  if (n == NULL) panic("out of bounds access %zu", index);
  if (n->type != SEXPR_ATOM) panic("list[%zu] not an atom", index);
  return (s_expr_atom_t*)n;
}


s_expr_list_t* s_expr_list_at(s_expr_list_t* list, usize index) {
  s_expr_t* n = s_expr_at(list, index);
  if (n == NULL) panic("out of bounds access %zu", index);
  if (n->type != SEXPR_LIST) panic("list[%zu] not a list", index);
  return (s_expr_list_t*)n;
}
