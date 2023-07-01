// SPDX-License-Identifier: Apache-2.0

// LOL, so okay, this is why I don't like glibc.
// For flockfile to be defined in glibc, _GNU_SOURCE must be defined
// and _POSIX_SOURCE must _not_ be defined.
#undef _POSIX_SOURCE

#include "colib.h"
#include <stdlib.h> // abort
#include <unistd.h> // close
#include <stdio.h>  // flockfile
#include <stdarg.h>

// backtrace* is not a libc standard.
// macOS has it, glibc has it (but we can't check at compile time)
#if defined(__MACH__) && defined(__APPLE__)
  #define HAS_BACKTRACE
  #include <execinfo.h>
#endif


void fprint_stacktrace(FILE* fp, int frame_offset) {
  #ifdef HAS_BACKTRACE

  void* buf[32];

  frame_offset++; // skip this function
  int framecount = backtrace(buf, countof(buf));
  if (framecount <= frame_offset)
    return;

  char** strs = backtrace_symbols(buf, framecount);
  if (strs != NULL) {
    for (; frame_offset < framecount; frame_offset++) {
      fwrite(strs[frame_offset], strlen(strs[frame_offset]), 1, fp);
      fputc('\n', fp);
    }
    free(strs);
  } else {
    fflush(fp);
    backtrace_symbols_fd(buf, framecount, fileno(fp));
  }

  #endif
}


void _panic(const char* file, int line, const char* fun, const char* fmt, ...) {
  FILE* fp = stderr;
  flockfile(fp);

  fprintf(fp, "\npanic: ");

  va_list ap;
  va_start(ap, fmt);
  vfprintf(fp, fmt, ap);
  va_end(ap);

  fprintf(fp, " (%s at %s:%d)\n", fun, file, line);

  fprint_stacktrace(fp, 1);

  funlockfile(fp);
  fflush(fp);
  fsync(STDERR_FILENO);

  abort();
}
