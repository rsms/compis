// SPDX-License-Identifier: Apache-2.0
#include "colib.h"
#include "compiler.h"
#include "abuf.h"
#include "unicode.h"


// DEBUG_INDENT: define to debug indentation
//#define DEBUG_INDENT


// FMTCOL e.g. printf("animal=" FMTCOL_FMT("%s"), FMTCOL_ARG(3, "cat"))
#define FMTCOL_FMT(pat)    "\e[9%cm" pat "\e[39m"
#define FMTCOL_ARG(n, val) ('1'+(char)((n)%6)), (val)


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
  u32 loc_srcfileid = locmap_intern_srcfileid(
    &s->compiler->locmap, srcfile, s->compiler->ma);
  s->loc = loc_make(loc_srcfileid, 1, 1, 1);
  s->lineno = 1;
  s->errcount = 0;
  s->err = 0;
  s->tok = TEOF;

  // s->indent = 0;
  s->indentdst = 0;
  s->indentstackv[0] = 0;
  s->indentstack = s->indentstackv;
}


void stop_scanning(scanner_t* s) {
  // move cursor to end of source causes scanner_next to return TEOF
  s->tok = TEOF;
  s->inp = s->inend;
  s->tokstart = s->inend;
  s->tokend = s->inend;
  s->insertsemi = false;
}


static usize litlen(const scanner_t* s) {
  assertf((uintptr)s->inp >= (uintptr)s->tokstart,
    "inp offset (%zu) >= tokstart offset (%zu)",
    (uintptr)(s->inp - (u8*)s->srcfile->data),
    (uintptr)(s->tokstart - (u8*)s->srcfile->data));

  return (usize)(uintptr)(s->inp - s->tokstart);
}


slice_t scanner_lit(const scanner_t* s) {
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
  s->errcount++;
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
  s->errcount++;
  stop_scanning(s);
}


static void out_of_mem(scanner_t* s) {
  if (s->err == 0)
    s->err = ErrNoMem;
  error(s, "out of memory");
}


inline static void newline(scanner_t* s) {
  assert(*s->inp == '\n');
  s->lineno++;
  s->linestart = s->inp + 1;
}


static void prepare_litbuf(scanner_t* s, usize minlen) {
  buf_clear(&s->litbuf);
  if UNLIKELY(!buf_reserve(&s->litbuf, minlen))
    out_of_mem(s);
}


// hexdecode decodes a hexadecimal string, e.g. "0153beef" => 0x153beef
static u64 hexdecode(const u8* p, usize len) {
  assert(len <= 16);
  u64 v = 0;
  p += len - 1;
  for (usize i = 0; i < len; i++) {
    u8 c = *(p - i);
    v |= (u64)(
      (c - '0') -
      ( (u8)('a' <= c && c <= 'f') * (('a' - 10) - '0') ) -
      ( (u8)('A' <= c && c <= 'F') * (('A' - 10) - '0') )
    ) << (i * 4);
  }
  return v;
}


// charlit     = "'" ( <any byte except "'" and control chars> | char_esc ) "'"
// char_esc    = byte_esc | unicode_esc
// byte_esc    = "\x" digit16 digit16
// unicode_esc = ( "\u" digit16{4} | "\U" digit16{8} )
// digit16     = 0-9 | A-F | a-f
//
// e.g.
//   'a'       u8  0x61
//   '\xff'    u8  0xff
//   '\u0153'  u32 0x153
//   '→'       u32 0x2192  (NOT the UTF-8 bytes 0xE2 0x86 0x92)
static void charlit(scanner_t* s) {
  s->tok = TCHARLIT;
  s->insertsemi = true;

  if (s->inp == s->inend)
    goto err;

  if (*s->inp != '\\') {
    // literal, e.g. 'a' => 0x61
    // Note: utf8_decode always advances s->inp by at least 1 byte.
    if (*s->inp < RUNE_SELF) {
      s->litint = *s->inp++;
    } else {
      s->litint = utf8_decode(&s->inp, s->inend);
      if UNLIKELY(s->litint == RUNE_INVALID) {
        loc_set_col(&s->loc, (u32)(uintptr)(s->inp - s->linestart) + 1);
        return error(s, "invalid UTF-8 data at byte offset %zu",
          (usize)(uintptr)((void*)s->inp - s->srcfile->data));
      }
    }
  } else {
    // escape sequence, e.g. '\n' => 0x0a
    s->inp++; // consume "\"
    if (s->inp == s->inend)
      goto err;
    usize ndigits;
    switch (*s->inp++) {
      case '"': case '\'': case '\\': s->litint = *(s->inp - 1); goto end;
      case '0': s->litint = (u8)'\0'; goto end;
      case 'a': s->litint = (u8)'\a'; goto end;
      case 'b': s->litint = (u8)'\b'; goto end;
      case 't': s->litint = (u8)'\t'; goto end;
      case 'n': s->litint = (u8)'\n'; goto end;
      case 'v': s->litint = (u8)'\v'; goto end;
      case 'f': s->litint = (u8)'\f'; goto end;
      case 'r': s->litint = (u8)'\r'; goto end;
      case 'x': ndigits = 2; break; // \xXX
      case 'u': ndigits = 4; break; // \uXXXX
      case 'U': ndigits = 8; break; // \UXXXXXXXX
      default:
        loc_set_col(&s->loc, (u32)(uintptr)(s->inp - s->linestart));
        return error(s, "invalid character-escape sequence");
    }

    // parse hexadecimal integer string, e.g. BEEF95
    if UNLIKELY(s->inp+ndigits >= s->inend || s->inp[ndigits] != '\'') {
      // TODO: catch common case of forgetting leading zeroes, e.g. '\u153'
      goto err;
    }
    s->litint = hexdecode(s->inp, ndigits);
    s->inp += ndigits;

    // validate
    if UNLIKELY((ndigits == 2 && s->litint >= RUNE_SELF) || !rune_isvalid(s->litint)) {
      u32 srcwidth = (u32)(uintptr)(s->inp - s->tokstart);
      if (s->inp < s->inend && *s->inp == '\'')
        srcwidth++; // include terminating "'" in source location
      loc_set_width(&s->loc, srcwidth);
      if (ndigits == 2) {
        // e.g. '\xff'
        error(s, "invalid UTF-8 sequence");
        origin_t origin = origin_make(&s->compiler->locmap, s->loc);
        if (origin.line) {
          report_diag(s->compiler, origin, DIAG_HELP,
            "If you want a raw byte value, write it as: 0x%02llX", s->litint);
          report_diag(s->compiler, origin, DIAG_HELP,
            "If you want a Unicode codepoint, write it as: '\\u%04llX'", s->litint);
        }
      } else {
        error(s, "invalid Unicode codepoint U+%04llX", s->litint);
      }
    }
  }

end:
  // expect terminating "'"
  if (*s->inp == '\'') {
    s->inp++;
    loc_set_width(&s->loc, (u32)(uintptr)(s->inp - s->tokstart));
    return;
  }

err:
  loc_set_col(&s->loc, (u32)(uintptr)(s->inp - s->linestart) + 1);
  if (s->inp < s->inend && *s->inp != '\'') {
    if (*(s->inp - 1) == '\'') { // e.g. ''
      loc_set_col(&s->loc, (u32)(uintptr)(s->inp - s->linestart));
      return error(s, "empty character literal");
    }
    return error(s, "invalid character literal; expected \"'\"");
  }
  error(s, "unterminated character literal");
}


static void floatnumber(scanner_t* s, int base) {
  s->tok = TFLOATLIT;
  s->insertsemi = true;
  bool allowsign = false;
  bool allowdot = true;
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
      // e.g. "2e+9"
      allowsign = true;
      allowdot = false;
      break;
    case 'P':
    case 'p':
      if (base < 16)
        goto err;
      allowsign = true;
      break;
    case '+':
    case '-':
      if (!allowsign)
        goto err;
      break;
    case '_':
      continue;
    case '.':
      if (!allowdot)
        goto err;
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
    return out_of_mem(s);
  return;
err:
  loc_set_col(&s->loc, (u32)(uintptr)(s->inp - s->linestart) + 1);
  error(s, "invalid syntax");
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
      if (c == 'E' - ('A' - 10)) { // 'e' or 'E'
        s->inp = start_inp; // rewind
        return floatnumber(s, base);
      }
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
    return out_of_mem(s);

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


err_t co_strlit_check(const u8* src, usize* srclenp, usize* declenp) {
  *declenp = 0;
  err_t err = 0;
  usize extralen = 2; // opening and closing '"'
  const u8* srcstart = src;
  const u8* srcend = src + *srclenp;

  // smallest possible string literal is ""
  if UNLIKELY(*srclenp < 2)
    return ErrEnd;

  // consume opening '"'
  if UNLIKELY(*src++ != '"')
    return ErrInvalid;

  while (src < srcend) {
    switch (*src++) {
      case '\\':
        switch (*src++) {
          case '"': case '\'': case '\\':
          case '0':
          case 'a':
          case 'b':
          case 't':
          case 'n':
          case 'v':
          case 'f':
          case 'r':
            break;
          case 'x': // \xXX
            if UNLIKELY(src >= srcend-2 || !ishexdigit(src[0]) || !ishexdigit(src[1])) {
              dlog("invalid string literal: malformed \\xNN escape sequence");
              err = ErrInvalid;
              goto end;
            }
            src += 2;
            extralen += 2;
            break;
          case 'u': // \uXXXX
          case 'U': // \UXXXXXXXX
            // TODO not yet supported
            FALLTHROUGH;
          default:
            dlog("invalid string literal: unexpected escape \"\\%c\"", *src);
            err = ErrInvalid;
            goto end;
        }
        extralen++;
        break;
      case '"':
        assert((usize)(uintptr)(src - srcstart) >= extralen);
        *declenp = (usize)(uintptr)(src - srcstart) - extralen;
        goto end;
      case '\n':
        // this function does not currently support mutliline string literals
        src--;
        err = ErrNotSupported;
        goto end;
      case 0:
        src--;
        err = ErrInvalid;
        goto end;
    }
  }
  // if we get here, there was no terminating '"'
  err = ErrEnd;
end:
  *srclenp = (usize)(uintptr)(src - srcstart);
  return err;
}


static u8 read_u8hex(const u8 src[2]) {
  assert(ishexdigit(src[0]) && ishexdigit(src[1]));
  // // here's a neat trick to calculate the bit shift amount given a base:
  // const u32 base = ...;
  // u32 shift = "\0\1\2\4\7\3\6\5"[((0x17 * base) >> 5) & 7];
  // // e.g. base=16 => shift=4
  return (g_intdectab[src[0]] << 4) | g_intdectab[src[1]];
}


err_t co_strlit_decode(const u8* src, usize srclen, u8* dst, usize declen) {
  // Note: srclen and declen are results from a successful call to co_strlit_check
  assert(srclen > 1);
  assert(*src == '"');
  assert(src[srclen-1] == '"');

  if (declen == 0)
    return 0;

  src++;                              // he... in '"hello"'
  const u8* end = src + (srclen - 2); // ...lo in '"hello"'
  const u8* chunkstart = src;

  #ifdef DEBUG
  const u8* dstend = dst + declen;
  #endif

  #define FLUSH(end) { \
    usize nbyte = (usize)((end) - chunkstart); \
    memcpy(dst, chunkstart, nbyte); \
    assert(dst + nbyte <= dstend); \
    dst += nbyte; \
  }

  while (src < end) {
    if (*src == '\\') {
      FLUSH(src);
      src++;
      assert(dst < dstend);
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
          assertf(src <= end-3, "%p <= %p-3(%p)", src, end, end-3);
          assertf(ishexdigit(src[1]) && ishexdigit(src[2]),
            "0x%02x '%c', 0x%02x '%c'", src[1], src[1], src[2], src[2]);
          *dst++ = read_u8hex(src+1);
          src += 2;
          break;
        // TODO: \uXXXX and \UXXXXXXXX
        default: UNREACHABLE; // co_strlit_check guards against this
      }
      src++;
      chunkstart = src;
    } else {
      // co_strlit_check guards against these cases
      assert(*src != '"');
      assert(*src != 0);
      assert(*src != '\n');
      src++;
    }
  }
  FLUSH(src);
  #undef FLUSH
  return 0;
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
    lit.len >= strlen(CO_ABI_GLOBAL_PREFIX) &&
    memcmp(CO_ABI_GLOBAL_PREFIX, lit.chars, strlen(CO_ABI_GLOBAL_PREFIX)) == 0)
  {
    return error(s,
      "invalid identifier; prefix '" CO_ABI_GLOBAL_PREFIX "' reserved for internal use");
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


static void parse_comment(scanner_t* s) {
  assert(s->inp+1 < s->inend);
  const u8* start = s->inp;
  loc_t start_loc = s->loc;

  if (s->inp[1] == '/') {
    // line comment "// ... <LF>"
    s->inp += 2;
    while (s->inp < s->inend && *s->inp != '\n')
      s->inp++;
  } else {
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

  #ifdef DEBUG
  if (opt_trace_scan) {
    char locstr[128];
    loc_fmt(s->loc, locstr, sizeof(locstr), &s->compiler->locmap);
    char litstr[128];
    string_repr(litstr, sizeof(litstr), start, (uintptr)(s->inp - start));
    _dlog(3, "S", __FILE__, __LINE__,
          "\e[2mCOMMENT     \e[0m \"%s\"\t%s", litstr, locstr);
  }
  #endif

  if (!s->parse_comments)
    return;

  // allocate comment
  memalloc_t ma = s->ast_ma ? s->ast_ma : s->compiler->ma;
  comment_t* c = mem_alloct(ma, comment_t);
  if (!c)
    return out_of_mem(s);
  c->bytes = start;
  c->len = (uintptr)(s->inp - start); assert(c->len > 2); // minimum: "//<LF>"
  c->loc = start_loc;
  if (loc_line(c->loc) == loc_line(s->loc))
    loc_set_width(&c->loc, loc_col(s->loc) - loc_col(c->loc));

  // group adjacent comments?
  comment_t* prev_comment =
    (s->comments.len > 0) ? s->comments.v[s->comments.len-1] : NULL;
  if (prev_comment &&
      loc_line(prev_comment->loc) == loc_line(s->loc) - 1 &&
      loc_col(prev_comment->loc) == loc_col(s->loc) &&
      prev_comment->bytes[1] == c->bytes[1])
  {
    // same type of comment with same indentation on adjacent lines
    assertnull(prev_comment->next);
    prev_comment->next = c;
  } else {
    // new comment
    if (!commentarray_push(&s->comments, s->compiler->ma, c))
      return out_of_mem(s);
  }
}


static void scan0(scanner_t* s);


static u8 char_at_offset(scanner_t* s, usize offs) {
  return *(s->inp + (offs * (usize)(s->inp+offs < s->inend)));
}


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
  case '~': s->tok = TTILDE; break;
  case '=': OP2( TASSIGN,  '=', TEQ); break;
  case '!': OP2( TNOT,     '=', TNEQ); break;
  case '*': OP2( TSTAR,    '=', TMULASSIGN); break;
  case '%': OP2( TPERCENT, '=', TMODASSIGN); break;
  case '^': OP2( TXOR,     '=', TXORASSIGN); break;
  case '<': switch (nextc) {
    case '<': switch (char_at_offset(s, 1)) {
      case '=': s->tok = TSHLASSIGN; s->inp += 2; break;
      default:  s->tok = TSHL; s->inp++;
    } break;
    case '=': s->tok = TLTEQ; s->inp++; break;
    default:  s->tok = TLT;
  } break;
  case '>': switch (nextc) {
    case '>': switch (char_at_offset(s, 1)) {
      case '=': s->tok = TSHRASSIGN; s->inp += 2; break;
      default:  s->tok = TSHR; s->inp++; s->insertsemi = true;
    } break;
    case '=': s->tok = TGTEQ; s->inp++; break;
    default: s->tok = TGT; s->insertsemi = true;
  } break;
  case '&': switch (nextc) {
    case '&': s->tok = TANDAND; s->inp++; break;
    case '=': s->tok = TANDASSIGN; s->inp++; break;
    default: s->tok = TAND;
  } break;
  case '|': switch (nextc) {
    case '|': s->tok = TOROR; s->inp++; break;
    case '=': s->tok = TORASSIGN; s->inp++; break;
    default: s->tok = TOR;
  } break;
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
      parse_comment(s);
      MUSTTAIL return scan0(s);
    case '=':
      ++s->inp, s->tok = TDIVASSIGN; break;
  } break;
  case '"': MUSTTAIL return string(s);
  case '\'': MUSTTAIL return charlit(s);
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
      s->insertsemi = true;
  } break;
  case '0': MUSTTAIL return zeronumber(s);
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


static void indent_error(scanner_t* s, u32 indent, const char* errmsg) {
  loc_set_width(&s->loc, indent);
  loc_set_line(&s->loc, s->lineno);
  loc_set_col(&s->loc, 1);
  return error(s, "%s", errmsg);
}


static void indent_error_mixed(scanner_t* s, const u8* p) {
  char want[4], got[4];

  abuf_t a = abuf_make(want, sizeof(want));
  abuf_repr(&a, s->linestart, 1);
  abuf_terminate(&a);

  a = abuf_make(got, sizeof(got));
  abuf_repr(&a, p, 1);
  abuf_terminate(&a);

  u32 indent = (u32)(uintptr)(s->inp - s->linestart);
  loc_set_width(&s->loc, indent);
  loc_set_line(&s->loc, s->lineno);
  loc_set_col(&s->loc, 1);

  error(s, "mixed indentation of '%s' and '%s' characters", want, got);
}


static void scan0(scanner_t* s) {
  // save for synthesize_token
  u32 prev_lineno = s->lineno;
  const u8* prev_linestart = s->linestart;

  // unwind indentation (in the process of dropping more than one indentation level)
  if (*s->indentstack > s->indentdst) {
indent_unwind:
    if (s->insertsemi) {
      s->tok = TSEMI;
      s->insertsemi = false;
    } else {
      assert(s->indentstack > s->indentstackv);
      s->indentstack--; // pop
      s->tok = TRBRACE;
      s->insertsemi = true;
      // Don't insert a trailing semicolon if the current token is the end of a group.
      // This allows us to write e.g.
      //   foo(fun (x int) int
      //     x * 2
      //   ) // <- "})" rather than "};)"
      if (*s->indentstack == s->indentdst && (*s->inp == ')' || *s->inp == ']'))
        s->insertsemi = false;
    }
    if (s->inp < s->inend)
      s->inp = s->linestart;
    goto synthesize_token;
  }

  // skip whitespace
  bool is_linestart = s->inp == s->linestart;
  while (s->inp < s->inend && isspace(*s->inp)) {
    if (*s->inp == '\n') {
      is_linestart = true;
      newline(s);
    }
    s->inp++;
  }

  if ((uintptr)(s->inend - s->inp) > 2 &&
      s->inp[0] == '/' && (s->inp[1] == '/' || s->inp[1] == '*'))
  {
    // ignore comment
  } else if (is_linestart) {
    // layout algorithm, automatic insertion of ";" "{" and "}"
    //
    // We use a layout stack (s.indentstack) of increasing indentations.
    // The top indentation on the stack holds the current layout indentation.
    // The initial layout stack contains the single value 0 (never popped.)
    u32 indent = (u32)(uintptr)(s->inp - s->linestart);
    u32 currindent = *s->indentstack;

    // debug log linestart
    #ifdef DEBUG_INDENT
    {
      char tmpbuf[32];
      string_repr(tmpbuf, sizeof(tmpbuf), s->linestart, (usize)indent);
      dlog("linestart %s │\e[47;30;1m%s\e[0m│ (insertsemi=%s)",
        (indent > currindent ? "→" : indent < currindent ? "←" : "—"), tmpbuf,
        s->insertsemi ? "true" : "false");
    }
    #endif

    // check for mixed-character indentation
    for (const u8* p = &s->linestart[1]; p < s->inp; p++) {
      if UNLIKELY(*s->linestart != *p) {
        indent_error_mixed(s, p);
        break;
      }
    }

    // if indentation increased and s->tok is not an expression continuation, insert "{"
    // │if x > 2 +
    // │       3   // expression continuation from "+" (indentation ignored)
    // │  y        // indentation increased, insert "{" before "y"
    // │           // indentation decreased, insert ";" "}" and ";"
    if (indent > currindent) {
      // push indentation
      if UNLIKELY(s->indentstack >= s->indentstackv + countof(s->indentstackv))
        return indent_error(s, indent, "too many nested indented blocks");
      *(++s->indentstack) = indent; // push

      #ifdef DEBUG_INDENT
      dlog("indent push %u -> %u", s->indentdst, indent);
      #endif

      s->indentdst = indent;

      // produce a "{" token, unless s->tok is an expression continuation
      // Note: if s->insertsemi==false, then the token is an expression continuation
      if (s->insertsemi) {
        s->tok = TLBRACE;
        s->insertsemi = false;
        goto synthesize_token;
      }
    } else if (indent < currindent) {
      // Indentation decreased and s->tok is not "}"; insert "}".
      // set target indentation for indent_unwind

      #ifdef DEBUG_INDENT
      dlog("indent pop %u -> %u", s->indentdst, indent);
      #endif

      s->indentdst = indent;

      // check for unbalanced "partial" indentation reduction
      // e.g.
      //│foo    // [0]
      //│    x  // [4]
      //│  y    // [2] error: unbalanced indentation
      assert(s->indentstack > s->indentstackv);
      if UNLIKELY(indent > *(s->indentstack - 1))
        return indent_error(s, indent, "unbalanced indentation");

      // if the first char on this line is an explicit '}', then pop its indentation
      if (*s->inp == '}')
        s->indentstack--;

      // produce a ";" token if the previous token is not an expression continuation
      if (s->insertsemi) {
        s->tok = TSEMI;
        s->insertsemi = false;
        goto synthesize_token;
      }

      if (*s->inp != '}') {
        // If we get here, it is going to be a syntax error since we don't support
        // any syntax where an expression continuation terminates a block.
        // E.g. "(\n}" is invalid.
        // Produce a "}" anyhow to keep the scanner regular & sane:
        goto indent_unwind;
      }
    } else {
      if (s->insertsemi) {
        if (*s->indentstack > indent && s->inp < s->inend)
          s->inp = s->linestart;
        s->tok = TSEMI;
        s->insertsemi = false;
        goto synthesize_token;
      }
    }
  } else if (*s->inp == '}' && s->insertsemi) {
    // Insert ";" before "}", e.g. "{ 23 }" => "{ 23; }"
    // This guarantees that statements are _ended_ by semicolons (not _separated_ by.)
    s->tok = TSEMI;
    s->insertsemi = false;
    goto synthesize_token;
  }

  if LIKELY(s->inp < s->inend)
    MUSTTAIL return scan1(s);

  // EOF
  if (s->indentstack != s->indentstackv) {
    dlog("EOF: has indentation");
    s->indentdst = s->indentstackv[0];
    goto indent_unwind;
  }
  s->tokstart = s->inend;
  s->inp = s->inend;
  s->tok = TEOF;
  loc_set_line(&s->loc, s->lineno);
  loc_set_col(&s->loc, (u32)(uintptr)(s->tokstart - s->linestart) + 1);
  if (s->insertsemi) {
    s->tok = TSEMI;
    s->insertsemi = false;
  }
  return;

synthesize_token:
  //dlog("synthesize_token %s", tok_name(s->tok));
  s->tokstart = s->inp;
  s->tokend = s->inp;
  loc_set_line(&s->loc, prev_lineno);
  loc_set_col(&s->loc, (usize)(uintptr)(s->tokend - prev_linestart) + 1);
  return;
}


#if 0
static void scan0_no_ws_indent(scanner_t* s) {
  // save for TSEMI
  u32 prev_lineno = s->lineno;
  const u8* prev_linestart = s->linestart;

  // skip whitespace
  while (s->inp < s->inend && isspace(*s->inp)) {
    if (*s->inp == '\n') {
      newline(s);
    }
    s->inp++;
  }

  if (prev_linestart != s->linestart && s->insertsemi) {
    // insert a semicolon
    s->insertsemi = false;
    s->tok = TSEMI;
    loc_set_line(&s->loc, prev_lineno);
    loc_set_col(&s->loc, (usize)(uintptr)(s->tokend - prev_linestart) + 1);
  } else if UNLIKELY(s->inp >= s->inend) {
    goto eof;
  }

  MUSTTAIL return scan1(s);

eof:
  s->tokstart = s->inend;
  s->tok = TEOF;
  loc_set_line(&s->loc, s->lineno);
  loc_set_col(&s->loc, (u32)(uintptr)(s->tokstart - s->linestart) + 1);
  if (s->insertsemi) {
    s->tok = TSEMI;
    s->insertsemi = false;
  }
}
#endif


void scanner_next(scanner_t* s) {
  s->tokend = s->inp;

  s->endloc = s->loc;
  loc_set_width(&s->endloc, 0);
  loc_set_col(&s->endloc, (u32)(uintptr)(s->tokend - s->linestart) + 1);

  scan0(s);

  #ifdef DEBUG
  if (opt_trace_scan) {
    char locstr[128];
    loc_fmt(s->loc, locstr, sizeof(locstr), &s->compiler->locmap);

    slice_t lit = scanner_lit(s);
    char litstr[128];
    if (s->tok == TSEMI || s->tok == TLBRACE || s->tok == TRBRACE) {
      *litstr = 0;
    } else {
      string_repr(litstr, sizeof(litstr), lit.chars, lit.len);
    }

    _dlog(3, "S", __FILE__, __LINE__,
      FMTCOL_FMT("%-12s") " \"%s\"\t%llu\t0x%llx\t%s",
      FMTCOL_ARG(s->tok, tok_name(s->tok)), litstr, s->litint, s->litint, locstr);
  }
  #endif
}
