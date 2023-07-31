// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "loc.h"
ASSUME_NONNULL_BEGIN

// forward declarations
typedef struct compiler_ compiler_t;

// diaghandler_t is called when an error occurs. Return false to stop.
typedef struct diag_ diag_t;
typedef void (*diaghandler_t)(const diag_t*, void* nullable userdata);

// diagkind_t
typedef enum { DIAG_ERR, DIAG_WARN, DIAG_HELP } diagkind_t;

// diag_t
typedef struct diag_ {
  compiler_t* compiler; // originating compiler instance
  const char* msg;      // descriptive message including "srcname:line:col: type:"
  const char* msgshort; // short descriptive message without source location
  const char* srclines; // source context (a few lines of the source; may be empty)
  origin_t    origin;   // origin of error (.line=0 if unknown)
  diagkind_t  kind;
} diag_t;


void report_diagv(compiler_t*, origin_t, diagkind_t, const char* fmt, va_list);
static void report_diag(compiler_t* c, origin_t, diagkind_t, const char* fmt, ...)
  ATTR_FORMAT(printf,4,5);

//———————————————————————————————————————————————————————————————————————————————————————
// implementation

inline static void report_diag(
  compiler_t* c, origin_t origin, diagkind_t kind, const char* fmt, ...)
{
  va_list ap;
  va_start(ap, fmt);
  report_diagv(c, origin, kind, fmt, ap);
  va_end(ap);
}


ASSUME_NONNULL_END
