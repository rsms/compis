#include "colib.h"
#include "s-expr.h"


static void s_expr_diag_handler(const s_expr_diag_t* diag, void* nullable userdata) {
  buf_t* buf = userdata;
  if (buf) {
    buf_printf(buf, "%u:%u: %s\n", diag->line, diag->col, diag->message);
  } else {
    elog("[s-expr test] %u:%u: %s", diag->line, diag->col, diag->message);
  }
}


static s_expr_list_t* s_expr_must_parse(memalloc_t ma, const char* source) {
  s_expr_list_t* list;
  slice_t src = slice_cstr(source);
  err_t err = s_expr_parse(&list, src, ma, s_expr_diag_handler, NULL);
  assertf(err == 0, "s_expr_parse: %s", err_str(err));
  assertf(list->type == SEXPR_LIST, "%u", list->type); // root is always a list
  return list;
}


static void s_expr_must_fail_to_parse(
  memalloc_t ma, const char* source, buf_t* diagbuf)
{
  s_expr_list_t* list;
  slice_t src = slice_cstr(source);
  buf_clear(diagbuf);
  err_t err = s_expr_parse(&list, src, ma, s_expr_diag_handler, diagbuf);
  assertf(err != 0, "s_expr_parse did not fail with input:\n\t%s\n", source);
  assertf(err == ErrInvalid, "%s", err_str(err));
  bool ok = buf_nullterm(diagbuf); assert(ok);
}


static void test_s_expr_fmt(buf_t* buf, const s_expr_t* n, u32 flags) {
  buf_clear(buf);
  err_t err = s_expr_fmt(n, buf, flags);
  assertf(err == 0, "s_expr_fmt: %s", err_str(err));
}


#define assert_list_atom_at(list, index, expected_value) \
  assert_slice_eq(s_expr_atom_at((list), (index))->slice, slice_cstr(expected_value))


UNITTEST_DEF(s_expr_1_parse) {
  memalloc_t ma = memalloc_ctx();
  s_expr_list_t* list;

  list = s_expr_must_parse(ma, "hello");
  assertf(list->kind == '.', "%c", list->kind);
  assert_list_atom_at(list, 0, "hello");
  assert(s_expr_at(list, 1) == NULL);

  list = s_expr_must_parse(ma, "hello good-bye");
  assertf(list->kind == '.', "%c", list->kind);
  assert_list_atom_at(list, 0, "hello");
  assert_list_atom_at(list, 1, "good-bye");
  assert(s_expr_at(list, 2) == NULL);

  // explicit outer '(...)' should yield '(' kind of list
  list = s_expr_must_parse(ma, "(hello)");
  assertf(list->kind == '(', "%c", list->kind);
  assert_list_atom_at(list, 0, "hello");
  assert(s_expr_at(list, 1) == NULL);

  // explicit multiple outer '(...)' should yield '.' kind of list
  list = s_expr_must_parse(ma, "(hello) (world)");
  assertf(list->kind == '.', "%c", list->kind);
  { s_expr_list_t* list2 = s_expr_list_at(list, 0);
    assertf(list2->kind == '(', "%c", list2->kind);
    assert_list_atom_at(list2, 0, "hello");
  }
  { s_expr_list_t* list2 = s_expr_list_at(list, 1);
    assertf(list2->kind == '(', "%c", list2->kind);
    assert_list_atom_at(list2, 0, "world");
  }
  assert(s_expr_at(list, 2) == NULL);

  list = s_expr_must_parse(ma, "   hello   \t\n  (good bye)   ");
  assertf(list->kind == '.', "%c", list->kind);
  assert_list_atom_at(list, 0, "hello");
  { s_expr_list_t* list2 = s_expr_list_at(list, 1);
    assertf(list2->kind == '(', "%c", list2->kind);
    assert_list_atom_at(list2, 0, "good");
    assert_list_atom_at(list2, 1, "bye");
  }

  list = s_expr_must_parse(ma, "hello [world 123 foo/bar {456(X Y Z)}] a + c ()");
  assertf(list->kind == '.', "%c", list->kind);
  assert_list_atom_at(list, 0, "hello");
  { s_expr_list_t* list2 = s_expr_list_at(list, 1);
    assertf(list2->kind == '[', "%c", list2->kind);
    assert_list_atom_at(list2, 0, "world");
    assert_list_atom_at(list2, 1, "123");
    assert_list_atom_at(list2, 2, "foo/bar");
    { s_expr_list_t* list3 = s_expr_list_at(list2, 3);
      assertf(list3->kind == '{', "%c", list3->kind);
      assert_list_atom_at(list3, 0, "456");
      { s_expr_list_t* list4 = s_expr_list_at(list3, 1);
        assertf(list4->kind == '(', "%c", list4->kind);
        assert_list_atom_at(list4, 0, "X");
        assert_list_atom_at(list4, 1, "Y");
        assert_list_atom_at(list4, 2, "Z");
      }
    }
  }
  assert_list_atom_at(list, 2, "a");
  assert_list_atom_at(list, 3, "+");
  assert_list_atom_at(list, 4, "c");
  { s_expr_list_t* list2 = s_expr_list_at(list, 5);
    assertf(list2->kind == '(', "%c", list2->kind);
    assert(list2->head == NULL); // empty
  }

  // -- parsing should fail --
  buf_t diagbuf = buf_make(ma);

  s_expr_must_fail_to_parse(ma, "hello)", &diagbuf);
  assert_cstr_eq(diagbuf.chars, "1:7: extraneous ')'\n");

  s_expr_must_fail_to_parse(ma, "hello (good", &diagbuf);
  assert_cstr_eq(diagbuf.chars, "1:12: unterminated list, missing closing ')'\n");

  buf_dispose(&diagbuf);
}


UNITTEST_DEF(s_expr_2_fmt) {
  memalloc_t ma = memalloc_ctx();
  s_expr_list_t* list;
  buf_t buf = buf_make(ma);

  list = s_expr_must_parse(ma, "hello [world 123 foo/bar {456(X Y Z)}] a + c ()");
  assertf(list->kind == '.', "%c", list->kind);

  // "compact" plain formatting
  test_s_expr_fmt(&buf, SEXPR_TYPECAST(list), /*u32 flags*/0);
  assert_cstr_eq("hello [world 123 foo/bar {456 (X Y Z)}] a + c ()", buf.chars);

  // "pretty" formatting
  test_s_expr_fmt(&buf, SEXPR_TYPECAST(list), SEXPR_FMT_PRETTY);
  // dlog("%s", buf.chars);
  assert_cstr_eq(
    "hello\n"
    "[world 123 foo/bar\n"
    "  {456\n"
    "    (X Y Z)}]\n"
    "a\n"
    "+\n"
    "c\n"
    "()"
    "", buf.chars);

  s_expr_free(list, ma);

  // prettyprint helper
  buf_clear(&buf);
  err_t err = s_expr_prettyprint(&buf, slice_cstr(
    "(hello [world 123 foo/bar {456 (X Y Z)}] a + c ())"));
  assertf(err == 0, "%s", err_str(err));
  assert_cstr_eq(
    "(hello\n"
    "  [world 123 foo/bar\n"
    "    {456\n"
    "      (X Y Z)}]\n"
    "  a\n"
    "  +\n"
    "  c\n"
    "  ())"
    "", buf.chars);

  // "((x))" should linebreak after first "("
  list = s_expr_must_parse(ma, "((x))");
  test_s_expr_fmt(&buf, SEXPR_TYPECAST(list), SEXPR_FMT_PRETTY);
  assert_cstr_eq(
    "(\n"
    "  (x))"
    "", buf.chars);
  s_expr_free(list, ma);

  buf_dispose(&buf);
}
