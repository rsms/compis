#include "colib.h"


UNITTEST_DEF(slice_iterlines) {
  struct {
    const char* input;
    const char* expected_output[16];
  } samples[] = {

    // linebreak at the end yields no extra empty line
    // (note: we think of LF as "line terminator" not "line divider")
    { "a\n", { "a" } },

    // empty input yields one empty line
    { "", { "" } },

    // single linebreak as input yields one empty line (not two)
    { "\n", { "" } },
    { "\n ", { "", " " } },

    // input without linebreak yields one line
    { "no break", { "no break" } },

    // a more complex example
    { "  \nline 2\n\n line 4  \nline 5\n",
      { "  ",
        "line 2",
        "",
        " line 4  ",
        "line 5" } },
  };

  for (usize i = 0; i < countof(samples); i++) {
    slice_t input = slice_cstr(samples[i].input);
    //dlog("———— input ————\n%.*s\n————————————————", (int)input.len, input.chars);
    u32 line_idx = 0;
    for (slice_t line = {}; slice_iterlines(input, &line); ) {
      //dlog("%u>> '%.*s'", line_idx, (int)line.len, line.chars);

      // make we don't get more lines than expected
      assertf(samples[i].expected_output[line_idx] != NULL,
              "extra line:\n%.*s", (int)line.len, line.chars);

      // make sure the line content is what we expect
      slice_t expected_line = slice_cstr(samples[i].expected_output[line_idx]);
      assert_slice_eq(line, expected_line);

      line_idx++;
    }

    // make sure all expected lines have been seen
    assertf(samples[i].expected_output[line_idx] == NULL,
              "missing line:\n%s", samples[i].expected_output[line_idx]);
  }
}
