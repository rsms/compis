// experiments enabled via "//!experiment <name>"
// SPDX-License-Identifier: Apache-2.0
#pragma once

#define EXPERIMENTS_FOREACH(_) /* _(NAME, const char* description) */ \
  _(fun_in_struct,         "struct t { fun f() }") \
  _(shorthand_call_syntax, "'f arg' as alternative to 'f(arg)'") \
// END EXPERIMENTS_FOREACH


typedef struct experiments_t {
  #define _(NAME, ...) bool NAME;
  EXPERIMENTS_FOREACH(_)
  #undef _
} experiments_t;
