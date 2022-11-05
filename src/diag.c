// diagnostics reporting
#include "c0lib.h"
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
    case TEOF:      typ = "end of input";         break;
    case TID:       typ = "identifier";           break;
    case TINTLIT:
    case TFLOATLIT: typ = "number";               break;
    case TBYTELIT:  typ = "byte";   quote = '\''; break;
    case TSTRLIT:   typ = "string"; quote = '"';  break;
    default:
      abuf_c(&s, '\'');
      abuf_str(&s, tok_repr(t));
      abuf_c(&s, '\'');
      return abuf_terminate(&s);
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

  return abuf_terminate(&s);
}


char* tok_descrs(char* buf, usize cap, tok_t t, slice_t lit) {
  usize len = tok_descr(buf, cap, t, lit);
  if (len >= cap && cap > 4)
    memset(&buf[cap - 4], '.', 3);
  return buf;
}


static void add_srclines(compiler_t* c, srcrange_t origin, abuf_t* s) {
  const input_t* input = origin.focus.input;
  c->diag.srclines = "";

  if (abuf_avail(s) < 4 || origin.focus.line == 0 || input->data.size == 0)
    return;

  if (origin.start.line > 0 && origin.start.input != input)
    origin.start.line = 0;
  if (origin.end.line > 0 && origin.end.input != input)
    origin.end.line = 0;

  srcloc_t loc = origin.focus; // TODO: use start & end

  u32 nlinesbefore = 1;
  u32 nlinesafter = 0;
  u32 startline = loc.line - MIN(loc.line - 1, nlinesbefore);
  u32 endline = loc.line + nlinesafter + 1;
  u32 line = startline;

  // start & end of line
  const char* start = (const char*)input->data.p;
  const char* end = start;

  const char* srcend = start + input->data.size;

  // forward to startline
  for (;;) {
    if (*end == '\n') {
      if (--startline == 0) break; // found
      start = end + 1;
    }
    end++;
    if (end == srcend) {
      if (--startline == 0) break; // no trailing LF
      return; // not found
    }
  }

  c->diag.srclines = s->p;
  int ndigits = u32log10(endline);

  for (;;) {
    int len = (int)(end - start);
    if (line != loc.line) {
      abuf_fmt(s, "%*u   │ %.*s", ndigits, line, len, start);
    } else if (origin.end.line) {
      assert(origin.start.line > 0);
      assert(origin.start.col > 0);
      assert(origin.focus.col > 0);
      assert(origin.end.col > 0);
      abuf_fmt(s, "%*u → │ %.*s\n"
                  "%*s   │ %*s",
        ndigits, line, len, start, ndigits, "", (int)(origin.start.col-1), "");
      u32 c = origin.start.col - 1;
      if (origin.focus.col == origin.start.col || origin.focus.col == origin.end.col) {
        // focus point is at either start or end extremes; just draw a line
        while (c++ < origin.end.col-1)
         abuf_str(s, "~");
      } else {
        while (c++ < origin.focus.col-1)
          abuf_str(s, "~");
        abuf_str(s, "↑");
        for (; c < origin.end.col-1; c++)
          abuf_str(s, "~");
      }
    } else {
      abuf_fmt(s, "%*u → │ %.*s\n"
                  "%*s   │ %*s↑",
        ndigits, line, len, start, ndigits, "", (int)loc.col - 1, "");
    }

    if (end == srcend || ++line == endline)
      break;
    abuf_c(s, '\n');
    // find next line
    start = ++end;
    while (end != srcend) {
      if (*end == '\n')
        break;
      end++;
    }
  }
}


void report_errorv(compiler_t* c, srcrange_t origin, const char* fmt, va_list ap) {
  va_list ap2;
  buf_reserve(&c->diagbuf, 512);

  for (;;) {
    abuf_t s = abuf_make(c->diagbuf.p, c->diagbuf.cap);
    c->diag.msg = s.p;

    if (origin.focus.line > 0) {
      abuf_fmt(&s, "%s:%u:%u: ",
        origin.focus.input->name, origin.focus.line, origin.focus.col);
    } else if (origin.focus.input->name[0] != 0) {
      abuf_fmt(&s, "%s: ", origin.focus.input->name);
    }
    abuf_str(&s, "error: ");

    // short message starts after origin and status info
    c->diag.msgshort = s.p;

    // append main message
    va_copy(ap2, ap); // copy va_list since we might read it twice
    abuf_fmtv(&s, fmt, ap2);
    va_end(ap2);

    // separate message from srclines
    abuf_c(&s, '\0');

    // populate c->diag.srclines
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

  c->errcount++;
  c->diag.origin = origin;
  c->diaghandler(&c->diag, c->userdata);
}
