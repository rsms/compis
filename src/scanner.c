// SPDX-License-Identifier: Apache-2.0
#include "colib.h"
#include "compiler.h"
#include "abuf.h"


// TRACE_TOKENS: define to trace each token scanned
//#define TRACE_TOKENS


static const struct { const char* s; u8 len; tok_t t; } keywordtab[] = {
  #define _(NAME, ...)
  #define KEYWORD(str, NAME) {str, (u8)strlen(str), NAME},
  #include "tokens.h"
  #undef _
  #undef KEYWORD
};


bool scanner_init(scanner_t* s, compiler_t* c) {
  memset(s, 0, sizeof(*s));
  s->compiler = c;
  buf_init(&s->litbuf, c->ma);

  // keywordtab must be sorted
  #if DEBUG
    for (usize i = 1; i < countof(keywordtab); i++)
      assertf(strcmp(keywordtab[i-1].s, keywordtab[i].s) < 0,
        "keywordtab out of order (%s)", keywordtab[i].s);
  #endif

  return true;
}


void scanner_dispose(scanner_t* s) {
  buf_dispose(&s->litbuf);
}


void scanner_begin(scanner_t* s, srcfile_t* srcfile) {
  assert(srcfile->size == 0 || srcfile->data != NULL);
  s->srcfile = srcfile;
  s->inp = srcfile->data;
  s->inend = srcfile->data + srcfile->size;
  s->linestart = srcfile->data;
  u32 loc_srcfileid = locmap_srcfileid(&s->compiler->locmap, srcfile, s->compiler->ma);
  s->loc = loc_make(loc_srcfileid, 1, 1, 1);
  s->lineno = 1;
}


void stop_scanning(scanner_t* s) {
  // move cursor to end of source causes scanner_next to return TEOF
  s->inp = s->inend;
  s->tok = TEOF;
  s->tokstart = s->inp;
  s->tokend = s->inp;
  s->insertsemi = false;
  scanner_next(s);
}


static usize litlen(const scanner_t* s) {
  return (usize)(uintptr)(s->inp - s->tokstart);
}


slice_t scanner_lit(const scanner_t* s) {
  assert((uintptr)s->inp >= (uintptr)s->tokstart);
  return (slice_t){
    .bytes = s->tokstart,
    .len = litlen(s),
  };
}


slice_t scanner_strval(const scanner_t* s) {
  if (s->litbuf.len > 0)
    return buf_slice(s->litbuf);
  slice_t str;
  str = scanner_lit(s);
  str.p++;
  str.len -= 2;
  return str;
}


ATTR_FORMAT(printf,2,3)
static void error(scanner_t* s, const char* fmt, ...) {
  if (s->inp == s->inend && s->tok == TEOF)
    return; // stop_scanning
  origin_t origin = origin_make(&s->compiler->locmap, s->loc);
  va_list ap;
  va_start(ap, fmt);
  report_diagv(s->compiler, origin, DIAG_ERR, fmt, ap);
  va_end(ap);
  stop_scanning(s);
}


ATTR_FORMAT(printf,3,4)
static void error_at(scanner_t* s, loc_t loc, const char* fmt, ...) {
  if (s->inp == s->inend && s->tok == TEOF)
    return; // stop_scanning
  origin_t origin = origin_make(&s->compiler->locmap, loc);
  va_list ap;
  va_start(ap, fmt);
  report_diagv(s->compiler, origin, DIAG_ERR, fmt, ap);
  va_end(ap);
  stop_scanning(s);
}


static void newline(scanner_t* s) {
  assert(*s->inp == '\n');
  s->lineno++;
  s->linestart = s->inp + 1;
}


static void prepare_litbuf(scanner_t* s, usize minlen) {
  buf_clear(&s->litbuf);
  if UNLIKELY(!buf_reserve(&s->litbuf, minlen))
    error(s, "out of memory");
}


static void floatnumber(scanner_t* s, int base) {
  s->tok = TFLOATLIT;
  s->insertsemi = true;
  bool allowsign = false;
  prepare_litbuf(s, 64);
  buf_push(&s->litbuf, '+');
  int ok = 1;
  if (*s->tokstart == '-')
    ok = buf_push(&s->litbuf, '-');
  if (base == 16)
    ok = buf_print(&s->litbuf, "0x");

  for (; s->inp != s->inend; ++s->inp) {
    switch (*s->inp) {
    case 'E':
    case 'e':
      allowsign = true;
      break;
    case 'P':
    case 'p':
      if (base < 16)
        goto end;
      allowsign = true;
      break;
    case '+':
    case '-':
      if (!allowsign)
        goto end;
      break;
    case '_':
      continue;
    case '.':
      allowsign = false;
      break;
    default:
      if (!isalnum(*s->inp))
        goto end;
      allowsign = false;
    }
    ok &= buf_push(&s->litbuf, *s->inp);
  }

end:
  ok &= buf_nullterm(&s->litbuf);
  if UNLIKELY(!ok)
    return error(s, "out of memory");
}


static void number(scanner_t* s, int base) {
  s->tok = TINTLIT;
  s->insertsemi = true;
  s->litint = 0;
  const u8* start_inp = s->inp;

  u64 cutoff = 0xffffffffffffffffllu; // U64_MAX
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
        if (base == 10 || base == 16) {
          s->inp = start_inp; // rewind
          return floatnumber(s, base);
        }
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
    error(s, "integer literal too large; overflows u64");
  if UNLIKELY(c == '_')
    error(s, "trailing \"_\" after integer literal");
}


static void zeronumber(scanner_t* s) {
  int base = 10;
  if (s->inp < s->inend) switch (*s->inp) {
    case 'X': case 'x': base = 16; s->inp++; break;
    case 'B': case 'b': base = 2;  s->inp++; break;
    case 'O': case 'o': base = 8;  s->inp++; break;
    case '.': s->inp--; return floatnumber(s, 10);
  }
  return number(s, base);
}


static usize string_multiline(scanner_t* s, const u8* start, const u8* end, loc_t loc) {
  if ((usize)(uintptr)(end - start) >= (usize)USIZE_MAX)
    return USIZE_MAX;

  if UNLIKELY(*start != '\n') {
    error(s, "multiline string must start with \"|\" on a new line");
    return USIZE_MAX;
  }

  u32 extralen = 0;
  const u8* src = start;
  const u8* ind = start;
  u32 indlen = 0;
  u32 lineno = loc_line(loc);
  u32 col = loc_col(loc);

  while (src != end) {
    if (*src++ != '\n') {
      col++;
      continue;
    }

    col = 1;
    lineno++;
    const u8* l = src;

    // find '|', leaving while loop with src positioned just after '|'
    while (src != end) {
      u8 c = *src++;
      if (c == '|') break;
      if UNLIKELY(c != ' ' && c != '\t') {
        loc_set_line(&loc, lineno);
        loc_set_col(&loc, col);
        error_at(s, loc, "missing \"|\" after linebreak in multiline string");
        return USIZE_MAX;
      }
    }

    u32 len = (u32)((src - 1) - l);
    extralen += len;

    if (indlen == 0) {
      indlen = len;
      ind = l;
    } else if UNLIKELY(indlen != len || memcmp(l, ind, len) != 0) {
      loc_set_line(&loc, lineno);
      loc_set_col(&loc, col);
      error_at(s, loc, "inconsitent indentation of multiline string");
      return USIZE_MAX;
    }
  }
  return extralen;
}


static void string_buffered(scanner_t* s, usize extralen, bool ismultiline, loc_t loc) {
  const u8* src = s->tokstart + 1;
  usize len = (usize)(uintptr)((s->inp - 1) - src);

  // calculate effective string length
  if (ismultiline) {
    if UNLIKELY(extralen >= len) {
      // string() assumes \n is followed by |, but it isn't the case.
      // i.e. a string of only linebreaks.
      return error(s, "missing \"|\" after linebreak in multiline string");
    }
    // verify indentation and calculate nbytes used for indentation
    usize indentextralen = string_multiline(s, src, src + len, loc);
    if UNLIKELY(indentextralen == USIZE_MAX) // an error occured
      return;
    if (check_add_overflow(extralen, indentextralen, &extralen))
      return error(s, "string literal too large");
    src++; len--;  // sans leading '\n'
  }
  assert(extralen <= len);
  len -= extralen;

  // allocate buffer
  buf_clear(&s->litbuf);
  u8* dst = buf_alloc(&s->litbuf, len);
  if UNLIKELY(!dst)
    return error(s, "out of memory");

  const u8* chunkstart = src;

  #define FLUSH_BUF(end) { \
    usize nbyte = (usize)((end) - chunkstart); \
    memcpy(dst, chunkstart, nbyte); \
    dst += nbyte; \
  }

  if (ismultiline) {
    while (*src++ != '|') {}
    chunkstart = src;
  }

  while (src < s->inend) {
    switch (*src) {
      case '\\':
        FLUSH_BUF(src);
        src++;
        switch (*src) {
          case '"': case '\'': case '\\': *dst++ = *src; break; // verbatim
          case '0':  *dst++ = (u8)0;   break;
          case 'a':  *dst++ = (u8)0x7; break;
          case 'b':  *dst++ = (u8)0x8; break;
          case 't':  *dst++ = (u8)0x9; break;
          case 'n':  *dst++ = (u8)0xA; break;
          case 'v':  *dst++ = (u8)0xB; break;
          case 'f':  *dst++ = (u8)0xC; break;
          case 'r':  *dst++ = (u8)0xD; break;
          case 'x': // \xXX
          case 'u': // \uXXXX
          case 'U': // \UXXXXXXXX
            // TODO: \x-style escape sequences
            return error(s, "string literal escape \"\\%c\" not yet supported", *src);
          default:
            return error(s, "invalid escape \"\\%c\" in string literal", *src);
        }
        chunkstart = ++src;
        break;
      case '\n':
        src++;
        FLUSH_BUF(src);
        // note: sstring_multiline has verified syntax already
        while (*src++ != '|') {
          assert(src < s->inend);
        }
        chunkstart = src;
        break;
      case '"':
        FLUSH_BUF(src);
        return;
      default:
        src++;
    }
  }
}


static void string(scanner_t* s) {
  s->tok = TSTRLIT;
  s->insertsemi = true;
  usize extralen = 0;
  bool ismultiline = false;
  loc_t loc = s->loc;

  buf_clear(&s->litbuf);

  while (s->inp < s->inend) {
    switch (*s->inp) {
      case '\\':
        s->inp++; // eat next byte
        extralen++;
        break;
      case '\n':
        newline(s);
        ismultiline = true;
        extralen++;
        break;
      case '"': {
        s->inp++;
        if (extralen || ismultiline)
          string_buffered(s, extralen, ismultiline, loc);
        return;
      }
    }
    s->inp++;
  }
  error(s, "unterminated string literal");
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


static void intern_identifier(scanner_t* s) {
  slice_t lit = scanner_lit(s);

  if UNLIKELY(
    lit.len >= strlen(CO_INTERNAL_PREFIX) &&
    memcmp(CO_INTERNAL_PREFIX, lit.chars, strlen(CO_INTERNAL_PREFIX)) == 0)
  {
    return error(s,
      "invalid identifier; prefix '" CO_INTERNAL_PREFIX "' reserved for internal use");
  }

  s->sym = sym_intern(lit.chars, lit.len);
  loc_set_width(&s->loc, lit.len);
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
  s->tok = TID;
  intern_identifier(s);
  // TODO utf8_len: loc_set_width(&s->loc, utf8_len);
}


static void maybe_keyword(scanner_t* s) {
  // binary search for matching keyword & convert currtok to keyword
  usize low = 0, high = countof(keywordtab), mid;
  slice_t lit = scanner_lit(s);
  int cmp;

  if (lit.len > KEYWORD_MAXLEN)
    return;

  while (low < high) {
    mid = (low + high) / 2;
    cmp = strncmp(lit.chars, keywordtab[mid].s, lit.len);
    // dlog("maybe_keyword %.*s <> %s = %d",
    //  (int)lit.len, lit.chars, keywordtab[mid].s, cmp);
    if (cmp == 0) {
      if (lit.len < keywordtab[mid].len) {
        high = mid;
      } else {
        s->tok = keywordtab[mid].t;
        break;
      }
    } else if (cmp < 0) {
      high = mid;
    } else {
      low = mid + 1;
    }
  }
}


static void identifier(scanner_t* s) {
  while (s->inp < s->inend && ( isalnum(*s->inp) || *s->inp == '_' ))
    s->inp++;
  if (s->inp < s->inend && (u8)*s->inp >= UTF8_SELF)
    return identifier_utf8(s);
  s->tok = TID;
  s->insertsemi = true;
  loc_set_width(&s->loc, scanner_lit(s).len);
  intern_identifier(s);
  maybe_keyword(s);
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


static void scan0(scanner_t* s);


static void scan1(scanner_t* s) {
  s->tokstart = s->inp;
  loc_set_line(&s->loc, s->lineno);
  loc_set_col(&s->loc, (u32)(uintptr)(s->tokstart - s->linestart) + 1);
  loc_set_width(&s->loc, 0);

  bool insertsemi = s->insertsemi;
  s->insertsemi = false;

  u8 c = *s->inp++; // load current char and advance input pointer
  u8 nextc = *(s->inp - (u8)(s->inp == s->inend)) * (u8)(s->inp != s->inend);

  #define OP2(tok1, c2, tok2) \
    ( s->tok = (nextc == c2) ? (++s->inp, tok2) : tok1 )

  switch (c) {
  case '(': s->tok = TLPAREN; break;
  case ')': s->insertsemi = true; s->tok = TRPAREN; break;
  case '{': s->tok = TLBRACE; break;
  case '}': s->insertsemi = true; s->tok = TRBRACE; break;
  case '[': s->tok = TLBRACK; break;
  case ']': s->insertsemi = true; s->tok = TRBRACK; break;
  case ':': s->tok = TCOLON; break;
  case ';': s->tok = TSEMI; break;
  case ',': s->tok = TCOMMA; break;
  case '?': s->tok = TQUESTION; break;
  case '!': s->tok = TNOT; break;
  case '~': s->tok = TTILDE; break;
  case '<': OP2( TLT,      '<', TSHL); break;
  case '>': OP2( TGT,      '>', TSHR); break;
  case '=': OP2( TASSIGN,  '=', TEQ); break;
  case '*': OP2( TSTAR,    '=', TMULASSIGN); break;
  case '%': OP2( TPERCENT, '=', TMODASSIGN); break;
  case '&': OP2( TAND,     '=', TANDASSIGN); break;
  case '|': OP2( TOR,      '=', TORASSIGN); break;
  case '^': OP2( TXOR,     '=', TXORASSIGN); break;
  case '+': switch (nextc) {
    case '+': s->insertsemi = true; s->tok = TPLUSPLUS; s->inp++; break;
    case '=': s->tok = TADDASSIGN; s->inp++; break;
    default: s->tok = TPLUS;
  } break;
  case '-': switch (nextc) {
    case '-': s->insertsemi = true; s->tok = TMINUSMINUS; s->inp++; break;
    case '=': s->tok = TSUBASSIGN; s->inp++; break;
    default: s->tok = TMINUS;
  } break;
  case '/': s->tok = TSLASH; switch (nextc) {
    case '/':
    case '*':
      s->inp--;
      s->insertsemi = insertsemi;
      skip_comment(s);
      MUSTTAIL return scan0(s);
    case '=':
      ++s->inp, s->tok = TDIVASSIGN; break;
  } break;
  case '0': MUSTTAIL return zeronumber(s);
  case '"': MUSTTAIL return string(s);
  case '.': switch (nextc) {
    case '0' ... '9':
      s->inp--;
      return floatnumber(s, 10);
    case '.':
      if (s->inp+1 < s->inend && s->inp[1] == '.') {
        s->inp += 2;
        s->tok = TDOTDOTDOT;
        break;
      }
      FALLTHROUGH;
    default:
      s->tok = TDOT;
  } break;
  default:
    if (isdigit(c)) {
      s->inp--;
      return number(s, 10);
    }
    if (c >= UTF8_SELF) {
      s->inp--; // identifier_utf8 needs to read c
      return identifier_utf8(s);
    }
    if LIKELY(isalpha(c) || c == '_')
      return identifier(s);

    dlog("unexpected input at srcfile.data[%zu] (srcfile.size=%zu)",
      (usize)(uintptr)((void*)s->inp - s->srcfile->data), s->srcfile->size);
    error(s, "unexpected input byte 0x%02X '%C'", c, c);
    stop_scanning(s);
  }
}


static void scan0(scanner_t* s) {
  // save for TSEMI
  u32 prev_lineno = s->lineno;
  const u8* prev_linestart = s->linestart;

  // skip whitespace
  while (s->inp < s->inend && isspace(*s->inp)) {
    if (*s->inp == '\n')
      newline(s);
    s->inp++;
  }

  // should we insert an implicit semicolon?
  if (prev_linestart != s->linestart && s->insertsemi) {
    s->insertsemi = false;
    s->tokstart = prev_linestart;
    s->tok = TSEMI;
    loc_set_line(&s->loc, prev_lineno);
    loc_set_col(&s->loc, (usize)(uintptr)(s->tokend - prev_linestart) + 1);
    return;
  }

  // EOF?
  if UNLIKELY(s->inp >= s->inend) {
    s->tokstart = s->inend;
    s->tok = TEOF;
    loc_set_line(&s->loc, s->lineno);
    loc_set_col(&s->loc, (u32)(uintptr)(s->tokstart - s->linestart) + 1);
    if (s->insertsemi) {
      s->tok = TSEMI;
      s->insertsemi = false;
    }
    return;
  }

  MUSTTAIL return scan1(s);
}


void scanner_next(scanner_t* s) {
  s->tokend = s->inp;
  scan0(s);

  #if defined(DEBUG) && defined(TRACE_TOKENS)
    char locstr[128];
    loc_fmt(s->loc, locstr, sizeof(locstr), &s->compiler->locmap);
    const char* name = tok_name(s->tok);
    slice_t lit = scanner_lit(s);
    _dlog(3, "S", __FILE__, __LINE__,
      "%-12s \"%.*s\"\t%llu\t0x%llx\t%s",
      name, (int)lit.len, lit.chars, s->litint, s->litint, locstr);
  #endif
}
