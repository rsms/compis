// diagnostics reporting
// SPDX-License-Identifier: Apache-2.0
#include "colib.h"
#include "compiler.h"
#include "abuf.h"
#include "path.h"


#define TAB_STR    "    "
#define TAB_WIDTH  strlen(TAB_STR)


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
      if (lit.len > 1) {
        lit.p++;
        lit.len -= 2;
      }
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


static slice_t replace_tabs(str_t* buf, const char* line, usize len, usize* ntabsp) {
  // find first tab
  u32 i = 0;
  while (i < len && line[i] != '\t')
    i++;

  *ntabsp = 0;

  // return a slice to line if no tabs were found
  if (i == len)
    return (slice_t){ .chars=line, .len=len };

  str_appendlen(buf, line, len);
  usize ntabs = str_replace(buf, slice_cstr("\t"), slice_cstr(TAB_STR), -1);
  if (ntabs < 0) {
    elog("ran out of memory while formatting diagnostic");
  } else {
    *ntabsp = (usize)ntabs;
  }

  return str_slice(*buf);
}


static void add_srcline_ctx(abuf_t* s, int lnw, u32 ln, slice_t line) {
  abuf_fmt(s, "%*u   │ ", lnw,ln);
  abuf_append(s, line.chars, line.len);
}


static void add_srcline(
  abuf_t* s, int lnw, u32 ln, slice_t line, origin_t origin)
{
  int indent = (int)origin.column - 1;
  if (origin.width == 0 && origin.focus_col > 0)
    indent = (int)origin.focus_col - 1;

  abuf_fmt(s, "%*u → │ ", lnw,ln);
  abuf_append(s, line.chars, line.len);
  abuf_fmt(s, "\n%*s   │ %*s", lnw,"", indent,"");

  if (origin.width == 0) {
    abuf_str(s, "↑");
    return;
  }

  if (origin.focus_col == 0) {
    abuf_fill(s, '~', origin.width);
    return;
  }

  u32 endcol = MAX(origin.focus_col, origin.column + origin.width);

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
    assert(endcol >= origin.focus_col);
    abuf_fill(s, '~', origin.width);
    abuf_fill(s, ' ', endcol - origin.focus_col);
    abuf_str(s, "↑");
  }
}


static void add_srclines(compiler_t* c, origin_t origin, abuf_t* s) {
  srcfile_t* srcfile = assertnotnull(origin.file);

  if (abuf_avail(s) < 4 || origin.line == 0 || srcfile->size == 0)
    return;

  err_t err = srcfile_open(srcfile);
  if (err) {
    dlog("srcfile_open(%s): %s", srcfile->name.p, err_str(err));
    return;
  }

  u32 nlinesbefore = 0; // TODO: make configurable
  u32 nlinesafter = 0; // TODO: make configurable
  u32 startline = origin.line - MIN(origin.line - 1, nlinesbefore);
  u32 endline = origin.line + nlinesafter + 1;
  u32 ln = startline;

  // start & end of line
  const char* p = (const char*)srcfile->data;
  const char* end = p;
  const char* srcend = p + srcfile->size;

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
  int lnw = (int)ndigits10((u64)endline);
  str_t buf = {0};

  for (;;) {
    buf.len = 0;
    usize line_rawlen = (usize)(uintptr)(end - p);
    usize ntabs;
    slice_t line = replace_tabs(&buf, p, line_rawlen, &ntabs);
    if (ntabs > 0) {
      // TODO: only increment columns for tabs in indentation.
      // Currently we (incorrectly) assume that tabs are only found in line indetation.
      // Doing this correctly will require knowledge of individual tab locations and
      // their effective column values.
      isize last_tab_idx = string_lastindexof(p, line_rawlen, '\t');
      //  →→foo
      //  ~~~^~
      //  01234
      //  ||
      //  12345
      if (origin.column > 1 && origin.column-1 <= last_tab_idx)
        origin.column += ntabs * (TAB_WIDTH-1);
      if (origin.column <= last_tab_idx+1 && origin.width > 1)
        origin.width += ntabs * (TAB_WIDTH-1);
      if (origin.focus_col > 1 && origin.focus_col-1 <= last_tab_idx)
        origin.focus_col += ntabs * (TAB_WIDTH-1);
    }

    if (ln != origin.line) {
      // context line
      add_srcline_ctx(s, lnw, ln, line);
      continue;
    }

    if (p + origin.column >= srcend)
      break;

    // origin line
    // (s, int lnw, u32 ln, slice_t line, origin_t origin)
    assert(p < srcend);
    assert(p + line_rawlen <= srcend);
    add_srcline(s, lnw, ln, line, origin);

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

  str_free(buf);
}


static void _report_diagv(
  compiler_t* c, origin_t origin, diagkind_t kind, const char* fmt, va_list ap)
{
  va_list ap2;
  buf_clear(&c->diagbuf);
  buf_reserve(&c->diagbuf, 1024);

  for (;;) {
    abuf_t s = abuf_make(c->diagbuf.p, c->diagbuf.cap);
    c->diag.msg = s.p;

    if (origin.file) {
      str_t filepath;
      if (origin.file->name.len > 0) {
        filepath = path_join(relpath(origin.file->pkg->dir.p), origin.file->name.p);
      } else {
        filepath = str_make("<input>");
      }
      if (origin.line > 0) {
        abuf_fmt(&s, "%s:%u:%u: ", filepath.p, origin.line, origin.column);
      } else if (origin.file->name.len > 0) {
        abuf_fmt(&s, "%s: ", filepath.p);
      }
      str_free(filepath);
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
    if (origin.file && origin.line)
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

  AtomicAdd(&c->errcount, (kind == DIAG_ERR ? 1u : 0u), memory_order_relaxed);
  c->diag.kind = kind;
  c->diag.origin = origin;
  c->diaghandler(&c->diag, c->userdata);
}


void report_diagv(
  compiler_t* c, origin_t origin, diagkind_t kind, const char* fmt, va_list ap)
{
  rwmutex_lock(&c->diag_mu);
  _report_diagv(c, origin, kind, fmt, ap);
  rwmutex_unlock(&c->diag_mu);
}
