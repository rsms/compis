#include "c0lib.h"
#include "compiler.h"
#include "abuf.h"


#define DEBUG_SCANNING
//#define PRODUCE_COMMENTS // produce TCOMMENT tokens instead of ignoring them


static void scan0(scanner_t* s);


void scanner_init(scanner_t* s, compiler_t* c) {
  memset(s, 0, sizeof(*s));
  s->compiler = c;
  indentarray_init(&s->indentstack, c->ma);
  buf_init(&s->litbuf, c->ma);
}


void scanner_dispose(scanner_t* s) {
  indentarray_dispose(&s->indentstack);
  buf_dispose(&s->litbuf);
}


void scanner_set_input(scanner_t* s, input_t* input) {
  s->input = input;
  s->inp = input->data.p;
  s->inend = input->data.p + input->data.size;
  s->linestart = input->data.p;
  s->tok.loc.line = 1;
  s->tok.loc.col = 1;
  s->tok.loc.input = input;
  s->lineno = 1;
  s->indentstack.len = 0;
}


static void stop_scanning(scanner_t* s) {
  // move cursor to end of source causes scanner_next to return TEOF
  s->inp = s->inend;
  s->tok.t = TEOF;
}


slice_t scanner_lit(const scanner_t* s) {
  assert((uintptr)s->inp >= (uintptr)s->tokstart);
  usize len = (usize)(uintptr)(s->inp - s->tokstart) - s->litlenoffs;
  return (slice_t){
    .bytes = s->tokstart,
    .len = len,
  };
}


ATTR_FORMAT(printf,2,3)
static void error(scanner_t* s, const char* fmt, ...) {
  srcrange_t range = {
    .focus = s->tok.loc,
  };
  va_list ap;
  va_start(ap, fmt);
  report_errorv(s->compiler, range, fmt, ap);
  va_end(ap);
  stop_scanning(s);
}


static void newline(scanner_t* s) {
  assert(*s->inp == '\n');
  s->lineno++;
  s->linestart = s->inp + 1;
}


static void indent_increase(scanner_t* s) {
  // dlog("[indent_increase] %u -> %u (%s)",
  //   s->indent.len, s->indentdst.len, s->indentdst.isblock ? "block" : "decorative");
  if (!indentarray_push(&s->indentstack, s->indent))
    return error(s, "out of memory");
  s->indent = s->indentdst;
}


static bool indent_decrease(scanner_t* s) {
  // dlog("[indent_decrease] %u -> %u (%s)",
  //   s->indent.len, s->indentdst.len, s->indent.isblock ? "block" : "decorative");
  bool isblock = s->indent.isblock;
  if (s->indentstack.len == 0) {
    s->indent = s->indentdst;
    return isblock;
  }
  s->indent = indentarray_pop(&s->indentstack);
  return isblock;
}


static void indent_error_mixed(scanner_t* s, const u8* p) {
  char want[4], got[4];

  abuf_t a = abuf_make(want, sizeof(want));
  abuf_repr(&a, s->linestart, 1);
  abuf_terminate(&a);

  a = abuf_make(got, sizeof(got));
  abuf_repr(&a, p, 1);
  abuf_terminate(&a);

  s->tokstart = s->inp;
  s->tok.loc.line = s->lineno;
  s->tok.loc.col = (u32)(uintptr)(s->tokstart - s->linestart) + 1;

  error(s, "mixed indentation: expected '%s', got '%s'", want, got);
}


static bool indent_check_mixed(scanner_t* s) {
  const u8* p = &s->linestart[1];
  u8 c = *s->linestart;
  while (p < s->inp) {
    if UNLIKELY(c != *p) {
      indent_error_mixed(s, p);
      return false;
    }
    p++;
  }
  return true;
}


static void floatnumber(scanner_t* s, int base) {
  s->tok.t = TFLOATLIT;
  bool allowsign = false;

  for (; s->inp != s->inend; ++s->inp) {
    switch (*s->inp) {
    case 'E':
    case 'e':
      allowsign = true;
      break;
    case 'P':
    case 'p':
      if (base < 16)
        return;
      allowsign = true;
      break;
    case '+':
    case '-':
      if (!allowsign)
        return;
      break;
    case '_':
    case '.':
      allowsign = false;
      break;
    default:
      if (!isalnum(*s->inp))
        return;
      allowsign = false;
    }
  }
}


static void number(scanner_t* s, int base) {
  s->tok.t = TINTLIT;
  s->insertsemi = true;
  s->litint = 0;

  u64 cutoff = 0xFFFFFFFFFFFFFFFFllu; // u64
  u64 acc = 0;
  u64 cutlim = cutoff % base;
  cutoff /= (u64)base;
  int any = 0;
  u8 c = *s->inp;

  for (; s->inp != s->inend; c = *++s->inp) {
    switch (c) {
      case '0' ... '9': c -= '0';      break;
      case 'A' ... 'Z': c -= 'A' - 10; break;
      case 'a' ... 'z': c -= 'a' - 10; break;
      case '_': continue; // ignore
      case '.':
        if (base == 10 || base == 16)
          return floatnumber(s, base);
        c = base; // trigger error branch below
        break;
      default:
        goto end;
    }
    if UNLIKELY(c >= base) {
      error(s, "invalid base-%d integer literal", base);
      return;
    }
    if (any < 0 || acc > cutoff || (acc == cutoff && (u64)c > cutlim)) {
      any = -1;
    } else {
      any = 1;
      acc *= base;
      acc += c;
    }
  }
end:
  s->litint = acc;
  if UNLIKELY(any < 0)
    error(s, "integer literal too large");
  if UNLIKELY(c == '_')
    error(s, "trailing \"_\" after integer literal");
}


static void zeronumber(scanner_t* s) {
  int base = 10;
  if (s->inp < s->inend) switch (*s->inp) {
    case 'X':
    case 'x':
      base = 16;
      s->inp++;
      break;
    case 'B':
    case 'b':
      base = 2;
      s->inp++;
      break;
    case 'O':
    case 'o':
      base = 8;
      s->inp++;
      break;
  }
  return number(s, base);
}


static bool utf8seq(scanner_t* s) {
  // TODO: improve this to be better and fully & truly verify UTF8
  u8 a = (u8)*s->inp++;
  if ((a & 0xc0) != 0xc0 || ((u8)*s->inp & 0xc0) != 0x80)
    return false;
  if (*s->inp++ == 0)   return false; // len<2
  if ((a >> 5) == 0x6)  return true;  // 2 bytes
  if (*s->inp++ == 0)   return false; // len<3
  if ((a >> 4) == 0xE)  return true;  // 3 bytes
  if (*s->inp++ == 0)   return false; // len<4
  if ((a >> 3) == 0x1E) return true;  // 4 bytes
  return false;
}


static void identifier_utf8(scanner_t* s) {
  while (s->inp < s->inend) {
    if ((u8)*s->inp >= UTF8_SELF) {
      if UNLIKELY(!utf8seq(s))
        return error(s, "invalid UTF8 sequence");
    } else if (isalnum(*s->inp) || *s->inp == '_') {
      s->inp++;
    } else {
      break;
    }
  }
  s->tok.t = TID;
}


static void identifier(scanner_t* s) {
  while (s->inp < s->inend && ( isalnum(*s->inp) || *s->inp == '_' ))
    s->inp++;
  if (s->inp < s->inend && (u8)*s->inp >= UTF8_SELF)
    return identifier_utf8(s);
  s->tok.t = TID;
  s->insertsemi = true;
}


static void eof(scanner_t* s) {
  s->tok.t = TEOF;
  s->indentdst.len = 0;

  if (s->indent.len > 0 && indent_decrease(s)) {
    // decrease indentation to 0 if source ends at indentation
    s->insertsemi = false;
    s->tok.t = TDEDENT;
  } else if (s->insertsemi) {
    s->insertsemi = false;
    s->tok.t = TSEMI;
  } else {
    s->tokstart = s->inend;
    s->tok.loc.line = s->lineno;
    s->tok.loc.col = (u32)(uintptr)(s->tokstart - s->linestart) + 1;
  }
}


inline static bool is_comment_start(scanner_t* s) {
  return
    s->inp+1 < s->inend &&
    (*s->inp == '/') & ((s->inp[1] == '/') | (s->inp[1] == '*'));
}


static void skip_comment(scanner_t* s) {
  assert(s->inp+1 < s->inend);
  u8 c = s->inp[1];

  if (c == '/') {
    // line comment "// ... <LF>"
    s->inp += 2;
    while (s->inp < s->inend && *s->inp != '\n')
      s->inp++;
    return;
  }

  // block comment "/* ... */"
  s->inp += 2;
  const u8* startstar = s->inp - 1; // make sure "/*/" != "/**/"
  while (s->inp < s->inend) {
    if (*s->inp == '\n') {
      newline(s);
    } else if (*s->inp == '/' && *(s->inp - 1) == '*' && s->inp - 1 != startstar) {
      s->inp++; // consume '*'
      break;
    }
    s->inp++;
  }
}


static void scan1(scanner_t* s) {
  s->tokstart = s->inp;
  s->tok.loc.line = s->lineno;
  s->tok.loc.col = (u32)(uintptr)(s->tokstart - s->linestart) + 1;

  bool insertsemi = s->insertsemi;
  s->insertsemi = false;

  u8 c = *s->inp++; // load current char and advance input pointer

  switch (c) {
  case '(': s->tok.t = TLPAREN; return;
  case ')': s->insertsemi = true; s->tok.t = TRPAREN; return;
  case '{': s->tok.t = TLBRACE; return;
  case '}': s->insertsemi = true; s->tok.t = TRBRACE; return;
  case '[': s->tok.t = TLBRACK; return;
  case ']': s->insertsemi = true; s->tok.t = TRBRACK; return;

  case ';': s->tok.t = TSEMI; return;
  case ',': s->tok.t = TCOMMA; return;
  case '+': s->tok.t = TPLUS; return;
  case '*': s->tok.t = TSTAR; return;
  case '%': s->tok.t = TPERCENT; return;
  case '&': s->tok.t = TAND; return;
  case '|': s->tok.t = TOR; return;
  case '^': s->tok.t = TXOR; return;
  case '~': s->tok.t = TTILDE; return;

  case '#': s->tok.t = THASH; return;
  case '<': s->tok.t = TLT; return;
  case '>': s->tok.t = TGT; return;

  case '0': return zeronumber(s);

  case '.':
    if (s->inp < s->inend) switch (*s->inp) {
    case '0' ... '9':
      s->inp--;
      return floatnumber(s, 10);
    case '.':
      s->tok.t = TDOTDOT;
      s->inp++;
      if (s->inp < s->inend && *s->inp == '.') {
        s->inp++;
        s->tok.t = TDOTDOTDOT;
      }
      return;
    }
    s->tok.t = TDOT;
    return;

  case '/':
    if (s->inp < s->inend && (*s->inp == '/' || *s->inp == '*')) {
      s->inp--;
      s->insertsemi = insertsemi;
      skip_comment(s);
      MUSTTAIL return scan0(s);
    }
    s->tok.t = TSLASH;
    return;

  default:
    if (isdigit(c)) {
      s->inp--;
      return number(s, 10);
    }
    if (c >= UTF8_SELF) {
      s->inp--; // identifier_utf8 needs to read c
      return identifier_utf8(s);
    }
    if (isalpha(c) || c == '_')
      return identifier(s);
    error(s, "unexpected input byte 0x%02X '%C'", c, c);
    stop_scanning(s);
  }
}


static void scan0(scanner_t* s) {
  s->litlenoffs = 0;

  // should we unwind >1-level indent?
  if (s->indent.len > s->indentdst.len && indent_decrease(s)) {
    s->tok.loc.col = (u32)(uintptr)(s->tokstart - s->linestart) + 1;
    s->tok.t = TDEDENT;
    return;
  }

  // are we at the start of a new line?
  bool is_linestart = s->inp == s->linestart;

  // save for TSEMI
  u32 prev_line = s->lineno;
  const u8* prev_linestart = s->linestart;

  // skip whitespace
  while (s->inp < s->inend && isspace(*s->inp)) {
    if (*s->inp == '\n') {
      newline(s);
      is_linestart = true;
    }
    s->inp++;
  }

  // should we insert an implicit semicolon or did indentation change?
  if (is_linestart) {
    indent_t indentdst = {
      .isblock = true,
      .len = (u32)(uintptr)(s->inp - s->linestart),
    };
    s->tokstart = s->linestart;

    if (indentdst.len > s->indent.len && !is_comment_start(s)) {
      s->indentdst = indentdst;
      indent_increase(s);
      //if (indentdst.isblock) {
      indent_check_mixed(s);
      s->insertsemi = false;
      s->tok.t = TINDENT;
      s->tok.loc.line = s->lineno;
      s->tok.loc.col = 1;
      return;
      //}
    }

    if (s->insertsemi) {
      s->insertsemi = false;
      s->tok.t = TSEMI;
      s->tok.loc.line = prev_line;
      s->tok.loc.col = (usize)(uintptr)(s->tokend - prev_linestart) + 1;
      return;
    }

    indent_check_mixed(s);
    if (indentdst.len < s->indent.len) {
      s->indentdst = indentdst;
      if (indent_decrease(s)) {
        s->insertsemi = false;
        s->tok.t = TDEDENT;
        s->tok.loc.line = s->lineno;
        s->tok.loc.col = 1;
        return;
      }
    }
  }

  if UNLIKELY(s->inp >= s->inend)
    MUSTTAIL return eof(s);

  MUSTTAIL return scan1(s);
}


void scanner_next(scanner_t* s) {
  s->tokend = s->inp;
  scan0(s);
  #ifdef DEBUG_SCANNING
    u32 line = s->tok.loc.line;
    u32 col = s->tok.loc.col;
    const char* srcfile = s->tok.loc.input->name;
    const char* name = tok_name(s->tok.t);
    slice_t lit = scanner_lit(s);
    log("scan> %s:%u:%u\t%-12s \"%.*s\"\t%llu\t0x%llx",
      srcfile, line, col, name, (int)lit.len, lit.chars, s->litint, s->litint);
  #endif
}
