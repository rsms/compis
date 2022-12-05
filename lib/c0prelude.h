#pragma once
#include <stdint.h>
#include <stdio.h>
#ifndef NULL
  #define NULL ((void*)0)
#endif
#define true 1
#define false 0
#define bool _Bool

__attribute__((noreturn)) void abort(void);
#define __nullcheck(x) ({ \
  __typeof__(x) x__ = (x); \
  (x__ == NULL ? abort() : ((void)0)), x__; \
})

// ——————————————————— internal ———————————————————————

inline static void _c0·drop(void* p) {
  printf("drop %p\n", p);
}
inline static void _c0·drop_opt(void* p) {
  if (p) _c0·drop(p);
}
