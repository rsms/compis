#include "colib.h"
#include "buf.h"


void _assert_slice_eq(
  slice_t a, slice_t b, const char* file, int line, const char* fun)
{
  if UNLIKELY(!slice_eq(a, b)) {
    buf_t buf = buf_make(memalloc_ctx());
    buf_print(&buf, "\n    \"");
    buf_appendrepr(&buf, a.p, a.len);
    buf_print(&buf, "\"\n != \"");
    buf_appendrepr(&buf, b.p, b.len);
    buf_print(&buf, "\"\n");
    if (buf_nullterm(&buf)) {
      _panic(file, line, fun, "Assertion failed: %s", buf.chars);
    } else {
      _panic(file, line, fun, "Assertion failed: \"%.*s\" != \"%.*s\"",
            (int)a.len, a.chars, (int)b.len, b.chars);
    }
    buf_dispose(&buf);
  }
}


slice_t slice_ltrim(slice_t s) {
  while (s.len > 0 && isspace(*s.chars)) {
    s.bytes++;
    s.len--;
  }
  return s;
}


slice_t slice_rtrim(slice_t s) {
  while (s.len > 0 && isspace(s.chars[s.len - 1]))
    s.len--;
  return s;
}


slice_t slice_trim(slice_t s) {
  return slice_ltrim(slice_rtrim(s));
}


bool slice_iterlines(slice_t source, slice_t* line) {
  const char* end = source.chars + source.len;

  if (line->chars == NULL) {
    // initialize
    assert(line->len == 0);
    *line = source;
  } else {
    // consume line + LF from last invocation
    line->chars += line->len + 1;
    if (line->chars < source.chars || line->chars >= end)
      return false;
  }

  // find next LF
  const char* p = line->chars;
  for (;;) {
    assert(p <= end);
    if (p == end || *p == '\n') {
      line->len = (uintptr)(p - line->chars);
      // return false for last line, e.g. "a\nb\n" yields "a" "b", not "a" "b" "".
      return line->len > 0 || p != end || source.len == 0;
    }
    p++;
  }
  UNREACHABLE;
}
