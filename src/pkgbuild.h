#pragma once
#include "compiler.h"
#include "bgtask.h"
#include "strlist.h"
ASSUME_NONNULL_BEGIN

// flags
#define PKGBUILD_NOLINK    (1u << 0)
#define PKGBUILD_NOCLEANUP (1u << 1) // skip cleanup
#define PKGBUILD_DEP       (1u << 2) // building a dependency (not a top-level) package
#define PKGBUILD_EXE       (1u << 3) // building an executable

typedef struct pkgcell_ pkgcell_t;
typedef struct pkgcell_ {
  pkgcell_t* nullable parent;
  pkg_t*              pkg;
} pkgcell_t;

typedef struct {
  pkgcell_t     pkgc;
  compiler_t*   c;
  bgtask_t*     bgt;
  memalloc_t    ast_ma;
  memalloc_t    api_ma;
  u32           flags;
  u32           unitc;
  unit_t**      unitv;
  strlist_t     cfiles;   // ".c" file paths, indexed by pkg->file id
  strlist_t     ofiles;   // ".o" file paths, indexed by pkg->file id
  promise_t*    promisev; // one promise for each srcfile, indexed by pkg->file id
  cgen_t        cgen;
  cgen_pkgapi_t pkgapi;
} pkgbuild_t;


err_t pkgbuild_init(
  pkgbuild_t* pb, pkgcell_t pkgc, compiler_t* c, memalloc_t api_ma, u32 flags);
void pkgbuild_dispose(pkgbuild_t* pb);

err_t pkgbuild_locate_sources(pkgbuild_t* pb);
err_t pkgbuild_begin_early_compilation(pkgbuild_t* pb);
err_t pkgbuild_parse(pkgbuild_t* pb);
err_t pkgbuild_import(pkgbuild_t* pb);
err_t pkgbuild_typecheck(pkgbuild_t* pb);
err_t pkgbuild_setinfo(pkgbuild_t* pb);
err_t pkgbuild_metagen(pkgbuild_t* pb);
err_t pkgbuild_cgen(pkgbuild_t* pb);
err_t pkgbuild_begin_late_compilation(pkgbuild_t* pb);
err_t pkgbuild_await_compilation(pkgbuild_t* pb);
err_t pkgbuild_link(pkgbuild_t* pb, const char* outfile/*can be empty*/);

// higher-level functionality
err_t build_toplevel_pkg(
  pkg_t* pkg, compiler_t* c, const char* outfile, u32 pkgbuild_flags);

ASSUME_NONNULL_END
