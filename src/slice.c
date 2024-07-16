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
