#include "c0lib.h"
#ifdef DEBUG

#include <stdarg.h>
#include <unistd.h> // isatty, close


static bool log_iscolor() {
  static int colors = 2;
  if (colors == 2)
    colors = !!isatty(2);
  return colors;
}


void _dlog(const char* file, int line, const char* fmt, ...) {
  FILE* fp = stderr;
  flockfile(fp);

  if (log_iscolor()) {
    const char* str = "\e[1;30m▍\e[0m";
    fwrite(str, strlen(str), 1, fp);
  } else {
    const char* str = "[D] ";
    fwrite(str, strlen(str), 1, fp);
  }

  va_list ap;
  va_start(ap, fmt);
  vfprintf(fp, fmt, ap);
  va_end(ap);

  if (log_iscolor()) {
    fprintf(fp, " \e[2m%s:%d\e[0m\n", file, line);
  } else {
    fprintf(fp, " (%s:%d)\n", file, line);
  }

  funlockfile(fp);
  fflush(fp);
}

#endif // DEBUG
