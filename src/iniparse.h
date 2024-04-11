// ini-style parser
// SPDX-License-Identifier: Apache-2.0
//
// Example:
//   const char* src =
//     "[section 1]\n"
//     "key 1 = value 1\n"
//     "key 2 = value 2\n"
//     "# comment\n"
//     "[section 2]\n"
//     "key 3: value 3\n";
//   iniparse_t p;
//   iniparse_begin(&p, src, strlen(src));
//   for (iniparse_result_t r; (r = iniparse_next(&p));) switch (r) {
//     case INIPARSE_SECTION: break; // name & namelen
//     case INIPARSE_VALUE: break;   // name & namelen, value & valuelen
//     case INIPARSE_COMMENT: break; // value & valuelen
//     case INIPARSE_ERR_SYNTAX: panic("<input>:%u: syntax error", p.srcline);
//     case INIPARSE_END: UNREACHABLE; break;
//   }
//
#pragma once
ASSUME_NONNULL_BEGIN

typedef struct {
  const char* src;
  const char* srcp;
  const char* srcend;
  u32         srcline;
  u32         namelen;  // length of name
  u32         valuelen; // length of value
  const char* name;
  const char* value;
} iniparse_t;

typedef enum {
  INIPARSE_END,        // end of input
  INIPARSE_SECTION,    // e.g. "[value]"
  INIPARSE_VALUE,      // e.g. "name=value"
  INIPARSE_COMMENT,    // e.g. "# value"
  INIPARSE_ERR_SYNTAX, // syntax error at iniparse_t.srcline
} iniparse_result_t;

void iniparse_begin(iniparse_t* p, const char* src, usize srclen);
iniparse_result_t iniparse_next(iniparse_t* p);

ASSUME_NONNULL_END
