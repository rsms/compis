// SPDX-License-Identifier: Apache-2.0
#include "colib.h"
#include "iniparse.h"


void iniparse_begin(iniparse_t* p, const char* src, usize srclen) {
  memset(p, 0, sizeof(*p));
  p->src = src;
  p->srcp = src;
  p->srcend = src + srclen;
  p->srcline = 1;
}


static iniparse_result_t iniparse_comment(iniparse_t* p) {
  // note: first ++srcp is to consume '#'
  p->value = p->srcp + 1;
  while (++p->srcp < p->srcend && *p->srcp != '\n') {}
  p->namelen = 0;
  p->valuelen = (u32)(uintptr)(p->srcp - p->value);
  return INIPARSE_COMMENT;
}


static iniparse_result_t iniparse_section(iniparse_t* p) {
  p->srcp++; // consume '['
  if (p->srcp == p->srcend || *p->srcp <= ' ')
    return INIPARSE_ERR_SYNTAX; // leading space or control char, e.g. "[ foo]"
  p->name = p->srcp;
  while (p->srcp < p->srcend) {
    if (*p->srcp == ']') {
      p->namelen = (u32)(uintptr)(p->srcp - p->name);
      p->valuelen = 0;
      p->srcp++;
      return INIPARSE_SECTION;
    }
    if UNLIKELY(*p->srcp == '\n')
      break;
    p->srcp++;
  }
  // unterminated, e.g. "[foo\n" or "[foo"
  return INIPARSE_ERR_SYNTAX;
}


static iniparse_result_t iniparse_entry_v1(iniparse_t* p) {
  p->name = p->srcp;
  p->value = p->srcp;
  p->valuelen = 0;
  const char* end;
  while (++p->srcp < p->srcend && *p->srcp != '\n') {
    if (*p->srcp == '=' || *p->srcp == ':') {
      end = p->srcp;
      while (--end > p->name && *end <= ' ') {}
      p->namelen = (u32)(uintptr)((end + 1) - p->name);
      while (++p->srcp < p->srcend && (*p->srcp == ' ' || *p->srcp == '\t')) {}
      p->value = p->srcp;
    }
  }
  end = p->srcp;
  if (p->name == p->value) {
    while (--end > p->name && *end <= ' ') {}
    p->namelen = (u32)(uintptr)((end + 1) - p->name);
  } else if (p->value < p->srcend) {
    while (--end > p->value && *end <= ' ') {}
    p->valuelen = (u32)(uintptr)((end + 1) - p->value);
  }
  return INIPARSE_VALUE;
}


static iniparse_result_t iniparse_entry(iniparse_t* p) {
  p->value = p->srcp;
  p->namelen = 0;
  const char* end;
  while (++p->srcp < p->srcend && *p->srcp != '\n') {
    if (*p->srcp == '\\') {
      p->srcp++;
      if (p->srcp == p->srcend)
        break;
    } else if (*p->srcp == '=' || *p->srcp == ':') {
      end = p->srcp;
      p->name = p->value;
      while (--end > p->name && *end <= ' ') {}
      p->namelen = (u32)(uintptr)((end + 1) - p->name);

      while (++p->srcp < p->srcend && (*p->srcp == ' ' || *p->srcp == '\t')) {}
      p->value = p->srcp;
      while (p->srcp < p->srcend && *p->srcp != '\n') p->srcp++;
      break;
    }
  }
  end = p->srcp;
  while (--end > p->value && *end <= ' ') {}
  p->valuelen = (u32)(uintptr)((end + 1) - p->value);
  return INIPARSE_VALUE;
}


iniparse_result_t iniparse_next(iniparse_t* p) {
  // skip control chars and whitespace
  while (p->srcp < p->srcend && *p->srcp <= ' ') {
    p->srcline += (u32)(*p->srcp == '\n');
    p->srcp++;
  }
  if (p->srcp >= p->srcend)
    return INIPARSE_END;
  if (*p->srcp == '[')
    MUSTTAIL return iniparse_section(p);
  if (*p->srcp == '#')
    MUSTTAIL return iniparse_comment(p);
  MUSTTAIL return iniparse_entry(p);
}


#ifdef CO_ENABLE_TESTS
UNITTEST_DEF(iniparse) {
  const char* src =
    "[section 1]\n"
    "  # comment 1\n"
    "  key1 = val1\n"
    "  key 2=val 2  "
    "\n"
    "[section 2]\n"
    "val3\\=k\n"
    "# comment 2\n"
    "key4: val4\n"
    "key5:\n"
    "val6"
    ;
  iniparse_t p;

  #if 0
  iniparse_begin(&p, src, strlen(src));
  for (iniparse_result_t r; (r = iniparse_next(&p));) switch (r) {
    case INIPARSE_SECTION:
      dlog("SECTION \"%.*s\"", (int)p.namelen, p.name);
      break;
    case INIPARSE_VALUE:
      dlog("VALUE   \"%.*s\" = \"%.*s\"",
        (int)p.namelen, p.name, (int)p.valuelen, p.value);
      break;
    case INIPARSE_COMMENT:
      dlog("COMMENT \"%.*s\"", (int)p.valuelen, p.value);
      break;
    case INIPARSE_ERR_SYNTAX: panic("<input>:%u: syntax error", p.srcline);
    case INIPARSE_END: UNREACHABLE; break;
  }
  #endif

  iniparse_begin(&p, src, strlen(src));

  #define assert_next_is_section(NAME) \
    ( assert(iniparse_next(&p) == INIPARSE_SECTION), \
      assert(p.namelen == strlen(NAME)), \
      assert(memcmp(p.name, NAME, p.namelen) == 0) )

  #define assert_next_is_comment(VALUE) \
    ( assert(iniparse_next(&p) == INIPARSE_COMMENT), \
      assert(p.valuelen == strlen(VALUE)), \
      assert(memcmp(p.value, VALUE, p.valuelen) == 0) )

  #define assert_next_is_value(NAME, VALUE) \
    ( assert(iniparse_next(&p) == INIPARSE_VALUE), \
      assert(p.namelen == strlen(NAME)), \
      assert(memcmp(p.name, NAME, p.namelen) == 0), \
      assert(p.valuelen == strlen(VALUE)), \
      assert(memcmp(p.value, VALUE, p.valuelen) == 0) )

  assert_next_is_section("section 1");
  assert_next_is_comment(" comment 1");
  assert_next_is_value("key1", "val1");
  assert_next_is_value("key 2", "val 2");
  assert_next_is_section("section 2");
  assert_next_is_value("", "val3\\=k");
  assert_next_is_comment(" comment 2");
  assert_next_is_value("key4", "val4");
  assert_next_is_value("key5", "");
  assert_next_is_value("", "val6");
  assert(iniparse_next(&p) == INIPARSE_END);
}
#endif // CO_ENABLE_TESTS
