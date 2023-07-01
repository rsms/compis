#pragma once
#include "compiler.h"
#include "bgtask.h"
#include "strlist.h"
ASSUME_NONNULL_BEGIN

// flags
#define PKGBUILD_NOLINK (1u << 0)

typedef struct {
  pkg_t*      pkg;
  compiler_t* c;
  bgtask_t*   bgt;
  memalloc_t  ast_ma;
  u32         flags;
  u32         unitc;
  unit_t**    unitv;
  strlist_t   cfiles;   // ".c" file paths, indexed by pkg->file id
  strlist_t   ofiles;   // ".o" file paths, indexed by pkg->file id
  promise_t*  promisev; // one promise for each srcfile, indexed by pkg->file id
} pkgbuild_t;


err_t pkgbuild_init(pkgbuild_t* pb, pkg_t* pkg, compiler_t* c, u32 flags);
void pkgbuild_dispose(pkgbuild_t* pb);

err_t pkgbuild_locate_sources(pkgbuild_t* pb);
err_t pkgbuild_begin_early_compilation(pkgbuild_t* pb);
err_t pkgbuild_parse(pkgbuild_t* pb);
err_t pkgbuild_typecheck(pkgbuild_t* pb);
err_t pkgbuild_cgen(pkgbuild_t* pb);
err_t pkgbuild_begin_late_compilation(pkgbuild_t* pb);
err_t pkgbuild_await_compilation(pkgbuild_t* pb);
err_t pkgbuild_link(pkgbuild_t* pb, const char* outfile/*can be empty*/);

ASSUME_NONNULL_END
