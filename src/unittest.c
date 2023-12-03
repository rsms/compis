// SPDX-License-Identifier: Apache-2.0
#include "colib.h"
#ifdef CO_ENABLE_TESTS

#include <unistd.h>


static unittest_t* testv[32] = {};
static u32         testc = 0;

static bool stderr_isatty = false;
static long stderr_fpos = 0;

static const char* waitstyle = "";
static const char* okstyle = "";
static const char* failstyle = "";
static const char* dimstyle = "";
static const char* nostyle = "";


void unittest_add(unittest_t* t) {
  assertf(countof(testv) > testc,
    "testv[%zu] too small; increase its size", countof(testv));
  testv[testc++] = t;
}


static int unittest_cmp(const void* x, const void* y, void* nullable ctx) {
  const unittest_t* a = *(unittest_t**)x;
  const unittest_t* b = *(unittest_t**)y;
  return strcmp(a->name, b->name);
}


static void print_status(unittest_t* t, bool done, const char* msg) {
  const char* marker_wait = "• ";
  const char* marker_ok   = "✓ ";
  const char* marker_fail = "✗ ";
  if (!stderr_isatty) {
    marker_wait = "";
    marker_ok   = "OK ";
    marker_fail = "FAIL ";
  }
  const char* status = done ? (t->failed ? marker_fail : marker_ok) : marker_wait;
  const char* style = done ? (t->failed ? failstyle : okstyle) : waitstyle;
  fprintf(stderr, "TEST %s%s%s%s %s%s:%d%s %s\n",
    style, status, t->name, nostyle,
    dimstyle, t->file, t->line, nostyle,
    msg);
}


u32 unittest_runall() {
  u32 nfail = 0;

  stderr_isatty = isatty(2);
  if (stderr_isatty) {
    waitstyle  = "";
    okstyle    = "\e[1;32m"; // green
    failstyle  = "\e[1;31m"; // red
    dimstyle   = "\e[2m";
    nostyle    = "\e[0m";
  }

  // sort tests by name, ascending
  co_qsort(testv, testc, sizeof(void*), unittest_cmp, NULL);

  // run tests
  for (u32 i = 0; i < testc; i++) {
    unittest_t* t = testv[i];

    print_status(t, /*done*/false, "...");
    if (stderr_isatty)
      stderr_fpos = ftell(stderr);

    u64 startat = nanotime();
    t->fn(t);
    // microsleep(400*1000);
    u64 timespent = nanotime() - startat;

    if (stderr_isatty) {
      long fpos = ftell(stderr);
      if (fpos == stderr_fpos) {
        // nothing has been printed since _testing_start_run; clear line
        // \33[A    = move to previous line
        // \33[2K\r = clear line
        fprintf(stderr, "\33[A\33[2K\r");
      }
    }

    char durbuf[25];
    fmtduration(durbuf, timespent);
    print_status(t, /*done*/true, durbuf);

    if (t->failed)
      nfail++;
  }

  // report failures after all tests has finished running
  if (nfail) {
    fprintf(stderr, "%sTEST FAILED:%s\n", failstyle, nostyle);
    for (u32 i = 0; i < testc; i++) {
      unittest_t* t = testv[i];
      if (t->failed)
        fprintf(stderr, "  %s\tat %s:%d\n", t->name, t->file, t->line);
    }
  }

  return nfail;
}


#endif // defined(CO_ENABLE_TESTS)
