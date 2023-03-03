// C builder
// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "compiler.h"
#include "strlist.h"
ASSUME_NONNULL_BEGIN

typedef u8 cobj_srctype_t;
enum cobj_srctype {
  COBJ_TYPE_C,        // .c, .C
  COBJ_TYPE_ASSEMBLY, // .s, .S
};

typedef u8 cobj_flags_t;
#define COBJ_EXCLUDE_FROM_LIB  (1<<0)  // don't include when linking library product

typedef struct {
  const char*         srcfile;
  char* nullable      objfile;
  strlist_t* nullable cflags;
  cobj_srctype_t      srctype;
  cobj_flags_t        flags;
} cobj_t;

typedef array_type(cobj_t) cobjarray_t;
DEF_ARRAY_TYPE_API(cobj_t, cobjarray)

typedef enum cbuild_kind {
  CBUILD_K_STATIC_LIB,
} cbuild_kind_t;

typedef struct {
  compiler_t*    c;
  strlist_t      cc;
  strlist_t      cc_snapshot;
  strlist_t      as;
  strlist_t      as_snapshot;
  cbuild_kind_t  kind;
  char*          name;
  const char*    srcdir;
  char* nullable objdir;
  cobjarray_t    objs;
  char           objfile[PATH_MAX];
} cbuild_t;

void cbuild_init(cbuild_t* b, compiler_t* c, const char* name);
void cbuild_dispose(cbuild_t* b);

cobj_t* nullable cbuild_add_source(cbuild_t* b, const char* srcfile);
void cobj_addcflagf(cbuild_t*, cobj_t*, const char* fmt, ...) ATTR_FORMAT(printf,3,4);
void cobj_setobjfilef(cbuild_t*, cobj_t*, const char* fmt, ...) ATTR_FORMAT(printf,3,4);

void cbuild_end_config(cbuild_t* b);
inline static bool cbuild_config_ended(cbuild_t* b) { return b->cc_snapshot.len != 0; }

err_t cbuild_build(cbuild_t* b, const char* outfile);

inline static bool cbuild_ok(cbuild_t* b) { return b->cc.ok & b->as.ok; }

ASSUME_NONNULL_END
