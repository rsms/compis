// diagnostics reporting
// SPDX-License-Identifier: Apache-2.0
#include "colib.h"
#include "compiler.h"
#include "abuf.h"


static u32 u32log10(u32 u) {
  return u >= 1000000000 ? 10 :
         u >= 100000000 ? 9 :
         u >= 10000000 ? 8 :
         u >= 1000000 ? 7 :
         u >= 100000 ? 6 :
         u >= 10000 ? 5 :
         u >= 1000 ? 4 :
         u >= 100 ? 3 :
         u >= 10 ? 2 :
         1;
}


const char* tok_name(tok_t t) {
  assert(t < TOK_COUNT);
  static const char* names[TOK_COUNT] = {
    #define _(NAME, ...) #NAME,
    #define KEYWORD(str, NAME) #NAME,
    #include "tokens.h"
    #undef _
    #undef KEYWORD
  };
  return names[t];
}


const char* tok_repr(tok_t t) {
  assert(t < TOK_COUNT);
  static const char* strings[TOK_COUNT] = {
    #define _(NAME, str) str,
    #define KEYWORD(str, NAME) str,
    #include "tokens.h"
    #undef _
    #undef KEYWORD
  };
  return strings[t];
}


usize tok_descr(char* buf, usize bufcap, tok_t t, slice_t lit) {
  abuf_t s = abuf_make(buf, bufcap);

  const char* typ;
  char quote = 0;

  switch (t) {
    case TEOF:      typ = "end of input"; break;
    case TID:       typ = "identifier"; quote = '"'; break;
    case TINTLIT:
    case TFLOATLIT: typ = "number"; break;
    case TBYTELIT:  typ = "byte";   quote = '\''; break;
    case TSTRLIT:   typ = "string"; quote = '"';
      lit.p++;
      lit.len -= 2;
      break;
    default:
      abuf_c(&s, '\'');
      abuf_str(&s, tok_repr(t));
      abuf_c(&s, '\'');
      goto end;
  }

  abuf_str(&s, typ);

  if (lit.len) {
    abuf_c(&s, ' ');
    if (quote) {
      abuf_c(&s, quote);
      abuf_repr(&s, lit.p, lit.len);
      abuf_c(&s, quote);
    } else {
      abuf_append(&s, lit.p, lit.len);
    }
  }

end:
  usize len = abuf_terminate(&s);
  if (len >= bufcap && bufcap > 4)
    memset(&buf[bufcap - 4], '.', 3);
  return len;
}


static void add_srcline_ctx(abuf_t* s, int lnw, u32 ln, const char* line, int linew) {
  abuf_fmt(s, "%*u   │ %.*s", lnw,ln, linew,line);
}


static void add_srcline(
  abuf_t* s, int lnw, u32 ln, const char* line, int linew, origin_t origin)
{
  int indent = (int)origin.column - 1;
  if (origin.width == 0 && origin.focus_col > 0)
    indent = (int)origin.focus_col - 1;

  abuf_fmt(s,
    "%*u → │ %.*s\n"
    "%*s   │ %*s",
    lnw,ln, linew,line, lnw,"", indent,"");

  if (origin.width == 0) {
    abuf_str(s, "↑");
    return;
  }

  if (origin.focus_col == 0) {
    abuf_fill(s, '~', origin.width);
    return;
  }

  u32 endcol = origin.column + origin.width;

  if (origin.focus_col < origin.column) {
    // focus point is before the source span, e.g.
    //   let foo = bar(1, 2, 3)
    //       ↑     ~~~
    abuf_str(s, "↑");
    // note: indentation printed so far is short, so fill up until origin.column
    abuf_fill(s, ' ', (origin.column - 1) - origin.focus_col);
    abuf_fill(s, '~', origin.width);
  } else if (origin.focus_col < endcol) {
    // focus point is inside the source span, e.g.
    //   let foo = bar(1, 2, 3)
    //                ~~~~^~~~~
    u32 leadw = origin.focus_col - origin.column;
    abuf_fill(s, '~', leadw);
    abuf_str(s, "^");
    abuf_fill(s, '~', (origin.width - 1) - leadw);
  } else {
    // focus point is after the source span, e.g.
    //   let foo = bar(1, 2, 3)
    //             ~~~    ↑
    abuf_fill(s, '~', origin.width);
    abuf_fill(s, ' ', endcol - origin.focus_col);
    abuf_str(s, "↑");
  }
}


static void add_srclines(compiler_t* c, origin_t origin, abuf_t* s) {
  const input_t* input = assertnotnull(origin.input);

  if (abuf_avail(s) < 4 || origin.line == 0 || input->data.size == 0)
    return;

  u32 nlinesbefore = 0; // TODO: make configurable
  u32 nlinesafter = 0; // TODO: make configurable
  u32 startline = origin.line - MIN(origin.line - 1, nlinesbefore);
  u32 endline = origin.line + nlinesafter + 1;
  u32 ln = startline;

  // start & end of line
  const char* p = (const char*)input->data.p;
  const char* end = p;
  const char* srcend = p + input->data.size;

  // forward to startline
  for (;;) {
    if (*end == '\n') {
      if (--startline == 0) break; // found
      p = end + 1;
    }
    end++;
    if (end == srcend) {
      if (--startline == 0) break; // no trailing LF
      return; // not found
    }
  }

  c->diag.srclines = s->p;
  int lnw = u32log10(endline);

  for (;;) {
    int linew = (int)(end - p);

    if (ln != origin.line) {
      // context line
      add_srcline_ctx(s, lnw, ln, p, linew);
      continue;
    }

    if (p + origin.column >= srcend)
      break;

    // origin line
    add_srcline(s, lnw, ln, p, linew, origin);

    // done?
    if (end == srcend || ++ln == endline)
      break;

    abuf_c(s, '\n');

    // find next line
    p = ++end;
    while (end != srcend) {
      if (*end == '\n')
        break;
      end++;
    }
  }
}


void report_diagv(
  compiler_t* c, origin_t origin, diagkind_t kind, const char* fmt, va_list ap)
{
  va_list ap2;
  buf_clear(&c->diagbuf);
  buf_reserve(&c->diagbuf, 1024);

  for (;;) {
    abuf_t s = abuf_make(c->diagbuf.p, c->diagbuf.cap);
    c->diag.msg = s.p;

    if (origin.line > 0 && origin.input) {
      abuf_fmt(&s, "%s:%u:%u: ", origin.input->name, origin.line, origin.column);
    } else if (origin.input && origin.input->name[0] != 0) {
      abuf_fmt(&s, "%s: ", origin.input->name);
    }

    switch (kind) {
      case DIAG_ERR:  abuf_str(&s, "error: "); break;
      case DIAG_WARN: abuf_str(&s, "warning: "); break;
      case DIAG_HELP: abuf_str(&s, "help: "); break;
    }

    // short message starts after origin and status info
    c->diag.msgshort = s.p;

    // append main message
    va_copy(ap2, ap); // copy va_list since we might read it twice
    abuf_fmtv(&s, fmt, ap2);
    va_end(ap2);

    // separate message from srclines
    abuf_c(&s, '\0');

    // populate c->diag.srclines
    c->diag.srclines = "";
    if (origin.input && origin.line)
      add_srclines(c, origin, &s);

    usize len = abuf_terminate(&s);
    if (len < c->diagbuf.cap) {
      // there was enough space in msgbuf; we are done
      break;
    }
    // increase the diagbuf capacity
    if (!buf_grow(&c->diagbuf, (len - c->diagbuf.cap) + 1)) {
      // failed to allocate memory; best effort: return partial message.
      // note that abuf has properly terminated the (partial) string already.
      break;
    }
  }

  c->errcount += (kind == DIAG_ERR);
  c->diag.kind = kind;
  c->diag.origin = origin;
  c->diaghandler(&c->diag, c->userdata);
}
