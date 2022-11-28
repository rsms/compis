#pragma once
#include <stdint.h>
#define NULL ((void*)0)
#define true 1
#define false 0
#define bool _Bool

__attribute__((noreturn)) void abort(void);
#define __nullcheck(x) ({ \
  __typeof__(x) x__ = (x); \
  (x__ == NULL ? abort() : ((void)0)), x__; \
})
