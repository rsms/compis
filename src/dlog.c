// SPDX-License-Identifier: Apache-2.0
#include "colib.h"
#ifdef DEBUG

#include <stdarg.h>
#include <unistd.h> // isatty, close


// POWERLINE: define to use "powerline" arrowhead
#define POWERLINE "\xEE\x82\xB0"  // U+E0B0


static bool log_iscolor() {
  static int colors = 2;
  if (colors == 2)
    colors = !!isatty(2);
  return colors;
}


void _dlog(
  int color, const char* nullable prefix,
  const char* file, int line,
  const char* fmt, ...)
{
  FILE* fp = stderr;
  flockfile(fp);

  if (log_iscolor()) {
    if (prefix && *prefix) {
      if (color < 0) {
        fprintf(fp, "\e[1;2m▍%s⟩\e[0m ", prefix);
      } else {
        u8 fg = 7; // white
        const char* fg_extra = "1;";
        if (color == 2 || color == 3 || color == 6 || color == 7) {
          fg = 0; // black
          fg_extra = ""; // no bold since it makes black grey
        }
        #ifdef POWERLINE
          fprintf(fp,
            "\e[4%um\e[%s3%um▍\e[3%um%s\e[0m\e[3%um" POWERLINE "\e[0m ",
            color, fg_extra, color, fg, prefix, color);
        #else
          fprintf(fp, "\e[4%um\e[%s3%um▍\e[3%um%s⟩\e[0m ",
            color, fg_extra, color, fg, prefix);
        #endif
      }
    } else if (color < 0) {
      const char* str = "\e[1;2m▍\e[0m";
      fwrite(str, strlen(str), 1, fp);
    } else {
      fprintf(fp, "\e[1;3%um▍\e[0m", color);
    }
  } else {
    if (!prefix || !*prefix)
      prefix = "D";
    fprintf(fp, "[%s] ", prefix);
  }

  va_list ap;
  va_start(ap, fmt);
  vfprintf(fp, fmt, ap);
  va_end(ap);

  if (log_iscolor()) {
    fprintf(fp, "  \e[2m%s:%d\e[0m\n", file, line);
  } else {
    fprintf(fp, " (%s:%d)\n", file, line);
  }

  funlockfile(fp);
  fflush(fp);
}

#endif // DEBUG
