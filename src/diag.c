// diagnostics reporting
// SPDX-License-Identifier: Apache-2.0
#include "colib.h"
#include "diag.h"
#include "compiler.h"
#include "abuf.h"
#include "path.h"
#include <unistd.h>
#include <stdlib.h>


#define TAB_STR    "    "
#define TAB_WIDTH  strlen(TAB_STR)


static bool g_enable_colors = false;


static void init_diag() {
  static bool g_one_time_init = false;
  if (g_one_time_init)
    return;
  g_one_time_init = true;
  const char* COMPIS_TERM_COLORS = getenv("COMPIS_TERM_COLORS");
  if (COMPIS_TERM_COLORS) {
    g_enable_colors = (*COMPIS_TERM_COLORS != '0' && *COMPIS_TERM_COLORS != 0);
  } else {
    g_enable_colors = !!isatty(STDERR_FILENO);
  }
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


static void add_srcline_ctx(abuf_t* s, int linew, u32 lineno, slice_t line) {
  if (g_enable_colors)
    abuf_str(s, "\e[2m"); // dimmed

  abuf_fmt(s, "%*u   │ ", linew, lineno);
  abuf_append(s, line.chars, line.len);

  if (g_enable_colors)
    abuf_str(s, "\e[0m"); // reset
}


static void add_srcline(
  abuf_t* s, int linew, u32 lineno, slice_t line, origin_t origin)
{
  if (origin.column == 0) {
    origin.width = 0;
    if (origin.focus_col > 0) {
      origin.column = origin.focus_col;
    } else {
      origin.column = 1;
    }
  }

  abuf_fmt(s, "%*u → │ ", linew, lineno);

  if (origin.column == 0 && origin.focus_col == 0)
    return;

  // fancy ANSI-style underline for range, when "colors" are enabled
  if (g_enable_colors && origin.column > 0 && origin.width > 0) {
    usize col1 = origin.column - 1;
    if (col1 >= line.len) {
      // this is some sort of bug; log, don't crash
      elog("BUG (%zu %zu) %s:%d", col1, line.len, __FILE__, __LINE__);
      abuf_append(s, line.chars, line.len);
      return;
    }

    // append first chunk of line, leading up to col1
    abuf_append(s, line.chars, col1);

    // start "bold" + "underline" style
    abuf_str(s, "\e[1;4m");

    // highlight focus column (when inside range)
    bool highlight_focus_col =
      origin.focus_col > col1 && origin.focus_col < col1 + origin.width;
    if (highlight_focus_col) {
      usize col2 = origin.focus_col - 1;
      abuf_append(s, line.chars + col1, (col2 - col1));
      abuf_str(s, "\e[37;44m"); // set fg=white & bg=blue
      abuf_append(s, line.chars + col2, 1);
      abuf_str(s, "\e[39;49m"); // reset fg & bg color
      abuf_append(s, line.chars + col2 + 1, origin.width - 1 - (col2 - col1));
    } else {
      // append second chunk
      abuf_append(s, line.chars + col1, origin.width);
    }

    // reset style
    abuf_str(s, "\e[0m");

    // append final chunk
    abuf_append(s, line.chars + col1 + origin.width, line.len - col1 - origin.width);

    // if there's no column to "focus" on (point an arrow to), we are done
    if (origin.focus_col == 0 || highlight_focus_col)
      return;
  } else {
    // no fancy styling
    abuf_append(s, line.chars, line.len);
    if (origin.column == 0)
      return;
  }

  // find indentation, which might be a mixture of TAB and SP
  usize indent_len = 0;
  for (; indent_len < line.len &&
         (line.chars[indent_len] == ' ' || line.chars[indent_len] == '\t');
         indent_len++)
  {}

  usize extra_indent_col;
  if (origin.width == 0 && origin.focus_col > origin.column) {
    extra_indent_col = origin.focus_col;
  } else {
    extra_indent_col = origin.column;
  }
  assert(extra_indent_col > 0);
  assert(extra_indent_col - 1 >= indent_len);
  usize extra_indent = (usize)extra_indent_col - 1 - indent_len;

  abuf_fmt(s, "\n%*s   │ %.*s", linew,"", (int)indent_len, line.chars);
  abuf_fill(s, ' ', extra_indent);

  // point to an interesting point
  if (origin.width == 0) {
    abuf_str(s, "↑");
    return;
  }

  // underline an interesting range
  if (origin.focus_col == 0) {
    // abuf_fill(s, '~', origin.width);
    for (usize n = origin.width; n--;)
      abuf_str(s, "▔");
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


static void add_srclines(compiler_t* c, origin_t origin, diagkind_t kind, abuf_t* s) {
  // note: we're doing a const cast for srcfile_open
  srcfile_t* srcfile = (srcfile_t*)assertnotnull(origin.file);

  if (abuf_avail(s) < 4 || origin.line == 0 || srcfile->size == 0)
    return;

  err_t err = srcfile_open(srcfile);
  if (err) {
    dlog("srcfile_open(%s): %s", srcfile->name.p, err_str(err));
    return;
  }

  u32 nlinesbefore = 1; // TODO: make configurable
  u32 nlinesafter = 1; // TODO: make configurable
  if (kind == DIAG_HELP) {
    nlinesbefore = 0;
    nlinesafter = 0;
  }

  assert(origin.line > 0);
  u32 startline = origin.line - MIN(origin.line - 1, nlinesbefore);
  u32 endline = origin.line + nlinesafter + 1;
  u32 lineno = startline;

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
  int linew = (int)ndigits10((u64)endline);
  str_t buf = {0};

  for (;;) {
    buf.len = 0;
    usize line_rawlen = (usize)(uintptr)(end - p);

    slice_t line = (slice_t){ .chars=p, .len=line_rawlen };

    if (lineno != origin.line) {
      // context line
      add_srcline_ctx(s, linew, lineno, line);
    } else if (p + origin.column >= srcend) {
      break;
    } else {
      // origin line
      // (s, int linew, u32 lineno, slice_t line, origin_t origin)
      assert(p < srcend);
      assert(p + line_rawlen <= srcend);
      add_srcline(s, linew, lineno, line, origin);
    }

    // done?
    if (end == srcend || ++lineno == endline)
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
  init_diag();

  va_list ap2;
  buf_clear(&c->diagbuf);
  buf_reserve(&c->diagbuf, 1024);

  for (;;) {
    abuf_t s = abuf_make(c->diagbuf.p, c->diagbuf.cap);
    c->diag.msg = s.p;

    if (origin.file) {
      str_t filepath;
      if (origin.file->name.len > 0) {
        if (origin.file->pkg) {
          filepath = path_join(relpath(origin.file->pkg->dir.p), origin.file->name.p);
        } else {
          filepath = str_make(relpath(origin.file->name.p));
        }
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
      add_srclines(c, origin, kind, &s);

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
