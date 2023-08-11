// SPDX-License-Identifier: Apache-2.0
#include "colib.h"
#include "pkgbuild.h"
#include "astencode.h"
#include "llvm/llvm.h"
#include "path.h"
#include "sha256.h"
#include "threadpool.h"

#include <sys/stat.h>


#define trace_import(fmt, va...) \
  _trace(opt_trace_import, 3, "import", "%*s" fmt, /*indent*/0, "", ##va)

#define trace_import_indented(indent, fmt, va...) \
  _trace(opt_trace_import, 3, "import", "%*s" fmt, (indent), "", ##va)


static err_t build_pkg(
  pkgcell_t pkgc, compiler_t* c, const char* outfile,
  memalloc_t api_ma, u32 pkgbuild_flags);

static void load_dependency(
  compiler_t* c, memalloc_t api_ma, const pkgcell_t* parent, pkg_t* pkg, bool sync);


err_t pkgbuild_init(
  pkgbuild_t* pb, pkgcell_t pkgc, compiler_t* c, memalloc_t api_ma, u32 flags)
{
  memset(pb, 0, sizeof(*pb));
  pb->pkgc = pkgc;
  pb->c = c;
  pb->api_ma = api_ma;
  pb->flags = flags;

  // package lives inside the builtins namespace
  pkgc.pkg->defs.parent = &c->builtins;

  // configure a bgtask for communicating status to the user
  int taskflags = (c->opt_verbose > 0) ? BGTASK_NOFANCY : 0;
  u32 tasklen = 1; // typecheck
  tasklen += (u32)!!c->opt_verbose; // metagen
  tasklen += (u32)!!c->opt_verbose; // cgen
  tasklen += (u32)!(flags & PKGBUILD_NOLINK); // link
  pb->bgt = bgtask_open(c->ma, pkgc.pkg->path.p, tasklen, taskflags);
  // note: bgtask_open currently panics on OOM; change that, make it return NULL

  // create AST allocator
  pb->ast_ma = memalloc_bump2(/*slabsize*/0, /*flags*/0);
  if (pb->ast_ma == memalloc_null()) {
    dlog("OOM: memalloc_bump_in_zeroed");
    return ErrNoMem;
  }

  strlist_init(&pb->cfiles, c->ma);
  strlist_init(&pb->ofiles, c->ma);

  return 0;
}


static void assert_promises_completed(pkgbuild_t* pb) {
  // catches missing (or broken) call to pkgbuild_await_compilation
  if (pb->promisev) for (u32 i = 0; i < pb->pkgc.pkg->srcfiles.len; i++)
    assertf(pb->promisev[i].await == NULL, "promisev[%u] was not awaited", i);
}


void pkgbuild_dispose(pkgbuild_t* pb) {
  // srcfiles may have been opened if diagnostics were reported during
  // typecheck or cgen, so let's make sure they are all closed
  for (u32 i = 0; i < pb->pkgc.pkg->srcfiles.len; i++)
    srcfile_close(pb->pkgc.pkg->srcfiles.v[i]);

  cgen_pkgapi_dispose(&pb->cgen, &pb->pkgapi);
  cgen_dispose(&pb->cgen);
  bgtask_close(pb->bgt);
  memalloc_bump2_dispose(pb->ast_ma);
  strlist_dispose(&pb->cfiles);
  strlist_dispose(&pb->ofiles);
  if (pb->promisev) {
    assert_promises_completed(pb);
    mem_freetv(pb->c->ma, pb->promisev, (usize)pb->pkgc.pkg->srcfiles.len);
  }
}


ATTR_FORMAT(printf,2,3)
static void pkgbuild_begintask(pkgbuild_t* pb, const char* fmt, ...) {
  pb->bgt->n++;
  va_list ap;
  va_start(ap, fmt);
  bgtask_setstatusv(pb->bgt, fmt, ap);
  va_end(ap);
}


static err_t dump_ast(const node_t* ast) {
  buf_t buf = buf_make(memalloc_ctx());
  err_t err = ast_repr(&buf, ast);
  if (!err) {
    fwrite(buf.chars, buf.len, 1, stderr);
    fputc('\n', stderr);
  }
  buf_dispose(&buf);
  return err;
}


static err_t dump_pkg_ast(const pkg_t* pkg, unit_t*const* unitv, u32 unitc) {
  buf_t buf = buf_make(memalloc_ctx());
  err_t err = ast_repr_pkg(&buf, pkg, (const unit_t*const*)unitv, unitc);
  if (!err) {
    fwrite(buf.chars, buf.len, 1, stderr);
    fputc('\n', stderr);
  }
  buf_dispose(&buf);
  return err;
}


static void build_ofiles_and_cfiles(pkgbuild_t* pb, str_t builddir) {
  str_t s = {0};

  for (u32 i = 0; i < pb->pkgc.pkg->srcfiles.len; i++) {
    // {builddir}/{srcfile}.o  (note that builddir includes pkgname)
    srcfile_t* srcfile = pb->pkgc.pkg->srcfiles.v[i];

    s.len = 0;

    if UNLIKELY(!str_ensure_avail(&s, builddir.len + 1 + srcfile->name.len + 2))
      goto oom;

    str_appendlen(&s, builddir.p, builddir.len);
    str_push(&s, PATH_SEPARATOR);
    str_appendlen(&s, srcfile->name.p, srcfile->name.len);

    str_append(&s, ".o");
    strlist_addlen(&pb->ofiles, s.p, s.len);

    s.p[s.len-1] = 'c';
    strlist_addlen(&pb->cfiles, s.p, s.len);
  }

  if (!pb->ofiles.ok || !pb->cfiles.ok)
    goto oom;
  str_free(s);
  return;
oom:
  str_free(s);
  panic("out of memory");
}


// prepare_builddir creates output dir and builds cfiles & ofiles
static err_t prepare_builddir(pkgbuild_t* pb) {
  str_t builddir = {0};
  if (!pkg_builddir(pb->pkgc.pkg, pb->c, &builddir))
    return ErrNoMem;
  err_t err = fs_mkdirs(builddir.p, 0770, FS_VERBOSE);
  if (pb->cfiles.len == 0)
    build_ofiles_and_cfiles(pb, builddir);
  str_free(builddir);
  return err;
}


static const char* ofile_of_srcfile_id(pkgbuild_t* pb, u32 srcfile_id) {
  assert(srcfile_id < pb->cfiles.len);
  char*const* ofiles = strlist_array(&pb->ofiles);
  return ofiles[srcfile_id];
}


static const char* cfile_of_srcfile_id(pkgbuild_t* pb, u32 srcfile_id) {
  assert(srcfile_id < pb->cfiles.len);
  char*const* cfiles = strlist_array(&pb->cfiles);
  return cfiles[srcfile_id];
}


static const char* cfile_of_unit(pkgbuild_t* pb, const unit_t* unit) {
  assert(pb->cfiles.len == pb->pkgc.pkg->srcfiles.len);
  assertnotnull(unit->srcfile);
  u32 srcfile_idx = ptrarray_rindexof(&pb->pkgc.pkg->srcfiles, unit->srcfile);
  assert(srcfile_idx < U32_MAX);
  return cfile_of_srcfile_id(pb, (u32)srcfile_idx);
}


// compile_c_source compiles a C source file in a background thread.
// Caller should await the provided promise.
static err_t compile_c_source(
  pkgbuild_t* pb, promise_t* promise, const char* cfile, const char* ofile)
{
  compiler_t* c = pb->c;

  // Use package as working directory for subprocesses.
  // ofile must not be relative because of this.
  assert(path_isabs(ofile));
  const char* wdir = pb->pkgc.pkg->dir.p;

  // subprocs attached to promise
  subprocs_t* subprocs = subprocs_create_promise(c->ma, promise);
  if (!subprocs)
    return ErrNoMem;

  // compile C -> object
  err_t err = compile_c_to_obj_async(c, subprocs, wdir, cfile, ofile);

  // compile C -> asm
  if (!err && c->opt_genasm)
    err = compile_c_to_asm_async(c, subprocs, wdir, cfile, ofile);

  if UNLIKELY(err)
    subprocs_cancel(subprocs);
  return err;
}


err_t pkgbuild_locate_sources(pkgbuild_t* pb) {
  pkg_t* pkg = pb->pkgc.pkg;

  if (pkg->srcfiles.len == 0)
    pkg_find_files(pkg);

  if (pkg->srcfiles.len == 0) {
    elog("[%s] no source files in %s", pkg->path.p, relpath(pkg->dir.p));
    return ErrNotFound;
  }

  // count number of compis source files
  u32 ncosrc = 0;
  for (u32 i = 0; i < pkg->srcfiles.len; i++) {
    srcfile_t* f = pkg->srcfiles.v[i];
    ncosrc += (u32)(f->type == FILE_CO);
  }

  // update bgtask
  pb->bgt->ntotal += pkg->srcfiles.len; // "parse foo.co"
  pb->bgt->ntotal += ncosrc;         // "compile foo.co"
  if (pb->c->opt_verbose)
    pb->bgt->ntotal += ncosrc;       // "cgen foo.co"

  // allocate promise array
  if (pb->promisev) {
    assert_promises_completed(pb);
    mem_freetv(pb->c->ma, pb->promisev, (usize)pkg->srcfiles.len);
  }
  pb->promisev = mem_alloctv(pb->c->ma, promise_t, (usize)pkg->srcfiles.len);
  if UNLIKELY(!pb->promisev) {
    pkgbuild_dispose(pb);
    return ErrNoMem;
  }

  return 0;
}


err_t pkgbuild_begin_early_compilation(pkgbuild_t* pb) {
  pkg_t* pkg = pb->pkgc.pkg;

  // find first C srcfile or bail out if there are no C sources in the package
  u32 i = 0;
  while (i < pkg->srcfiles.len) {
    srcfile_t* f = pkg->srcfiles.v[i];
    if (f->type == FILE_C)
      break;
    if (++i == pkg->srcfiles.len)
      return 0; // no C sources
  }

  err_t err = 0;

  // create output dir and build cfiles & ofiles
  if (( err = prepare_builddir(pb) ))
    return err;

  for (; i < pkg->srcfiles.len && err == 0; i++) {
    srcfile_t* srcfile = pkg->srcfiles.v[i];
    if (srcfile->type != FILE_C)
      continue;
    const char* cfile = srcfile->name.p;
    const char* ofile = ofile_of_srcfile_id(pb, i);
    pkgbuild_begintask(pb, "compile %s", relpath(cfile));
    err = compile_c_source(pb, &pb->promisev[i], cfile, ofile);
    if (err)
      dlog("compile_c_source: %s", err_str(err));
  }

  return err;
}


typedef struct {
  sema_t           sem;
  _Atomic(unit_t*) unit;
  _Atomic(err_t)   err;
} parseres_t;


static void parse_co_file(
  pkg_t*      pkg,
  compiler_t* c,
  srcfile_t*  srcfile,
  memalloc_t  ast_ma,
  parseres_t* result)
{
  err_t err;
  parser_t parser;

  if (( err = srcfile_open(srcfile) )) {
    elog("%s: %s", srcfile->name.p, err_str(err));
    goto end;
  }

  if (!parser_init(&parser, c)) {
    dlog("parser_init failed");
    err = ErrNoMem;
    goto end;
  }

  dlog_if(opt_trace_parse, "————————— parse %s —————————",
    relpath(path_join(pkg->dir.p, srcfile->name.p).p)); // leaking memory :-/

  unit_t* unit;
  err = parser_parse(&parser, ast_ma, srcfile, &unit);
  AtomicStoreRel(&result->unit, unit);

  if (!err && parser_errcount(&parser) > 0) {
    dlog("syntax errors");
    err = ErrCanceled;
  }

  if (opt_trace_parse && c->opt_printast) {
    dlog("————————— AST after parse —————————");
    dump_ast(assertnotnull((node_t*)result->unit));
  }

  parser_dispose(&parser);

end:
  srcfile_close(srcfile);
  AtomicStoreRel(&result->err, err);
  sema_signal(&result->sem, 1);
}


static err_t pkgbuild_parse(pkgbuild_t* pb) {
  compiler_t* c = pb->c;
  pkg_t* pkg = pb->pkgc.pkg;

  // count number of compis source files
  u32 ncosrc = 0;
  for (u32 i = 0; i < pkg->srcfiles.len; i++) {
    srcfile_t* f = pkg->srcfiles.v[i];
    ncosrc += (u32)(f->type == FILE_CO);
  }

  // allocate unit array
  unit_t** unitv = mem_alloctv(pb->ast_ma, unit_t*, ncosrc);
  if (!unitv) {
    dlog("mem_alloctv OOM");
    return ErrNoMem;
  }
  pb->unitv = unitv;
  pb->unitc = ncosrc;

  // allocate result array
  parseres_t result_st[8];
  parseres_t* resultv = result_st;
  if (ncosrc > countof(result_st)) {
    resultv = mem_alloctv(pb->c->ma, parseres_t, ncosrc);
    if UNLIKELY(!resultv) {
      dlog("mem_alloctv OOM");
      mem_freetv(pb->c->ma, unitv, ncosrc);
      pb->unitc = 0;
      return ErrNoMem;
    }
  }

  // parse each file
  err_t err = 0;
  for (u32 i = 0, resultidx = 0; i < pkg->srcfiles.len && err == 0; i++) {
    srcfile_t* srcfile = pkg->srcfiles.v[i];

    if (srcfile->type != FILE_CO) {
      assertf(srcfile->type == FILE_C, "%s: unrecognized file type", srcfile->name.p);
      continue;
    }

    pkgbuild_begintask(pb, "parse %s", relpath(srcfile->name.p));

    parseres_t* result = &resultv[resultidx++];
    result->unit = NULL;
    result->err = 0;
    safecheckx(sema_init(&result->sem, 0) == 0);

    if (opt_trace_parse || comaxproc == 1 || resultidx == ncosrc-1) {
      // Parse sources serially when tracing is enabled or if there're no threads.
      // Also, parse last one on the current thread to make the most of what we have.
      parse_co_file(pkg, c, srcfile, pb->ast_ma, result);
    } else {
      threadpool_submit(parse_co_file, pkg, c, srcfile, pb->ast_ma, result);
    }
  }

  // wait for results
  for (u32 i = 0; i < ncosrc; i++) {
    safecheckx(sema_wait(&resultv[i].sem));
    err_t err1 = AtomicLoadAcq(&resultv[i].err);
    if (err1 && !err) {
      err = err1;
    } else {
      unitv[i] = AtomicLoadAcq(&resultv[i].unit);
    }
    sema_dispose(&resultv[i].sem);
  }

  #if DEBUG
  for (u32 i = 0; i < pb->unitc && err == 0; i++)
    assertnotnull(pb->unitv[i]);
  #endif

  if (resultv != result_st)
    mem_freetv(pb->c->ma, resultv, ncosrc);

  if (err)
    return err;

  return err;
}


// check_pkg_src_uptodate stats pkg->files and compares their mtime to product_mtime.
// It also compares the names at pkg->files to readdir(pkg->dir).
// product_mtime should be the timestamp of a package product, like metafile or libfile.
// If a source file is newer than product_mtime, the set of files on disk has changed
// or an I/O error occurred, false is returned to signal ""
static bool check_pkg_src_uptodate(pkg_t* pkg, unixtime_t product_mtime) {
  bool ok = false;

  // First we need to scan for added or removed source files on disk.
  // Since we "own" pkg here, it's safe to modify its srcfiles array, which we'll
  // do in order to compare cached srcfiles vs actual on-disk srcfiles.

  ptrarray_t cached_srcfiles = {0};
  CO_SWAP(cached_srcfiles, pkg->srcfiles);

  // populate pkg->srcfiles with source files found on disk
  err_t err = pkg_find_files(pkg);
  if (err) {
    dlog("[%s] error in pkg_find_files: %s", __FUNCTION__, err_str(err));
    goto end;
  }

  // now pkg->srcfiles contains the current srcfiles on disk and cached_srcfiles
  // contains the sources used to build the existing product.

  // if the number of source files changed, the package is definitely out of date
  if (cached_srcfiles.len != pkg->srcfiles.len)
    goto end;

  // Find renamed, added, removed or modified files.
  // Since srcfilearray_t is sorted (by name) we can find name differences
  // simply by comparing file by file.
  // We also take this opportunity to check mtime.
  for (u32 i = 0; i < cached_srcfiles.len; i++) {
    srcfile_t* cached_srcfile = cached_srcfiles.v[i];
    srcfile_t* found_srcfile = pkg->srcfiles.v[i];
    if (!streq(cached_srcfile->name.p, found_srcfile->name.p)) {
      //dlog("newfound srcfile: %s", found_srcfile->name.p);
      goto end;
    }
    if (found_srcfile->mtime > product_mtime) {
      //dlog("modified srcfile: %s", pkg->srcfiles.v[i].name.p);
      goto end;
    }
  }

  // pkg is up-to-date; source files have not changed since product was created
  ok = true;

end:
  // note: we are NOT using srcfilearray_dispose here since that would dispose
  // of the srcfile structs as well, which are owned by the pkg->srcfiles array.
  ptrarray_dispose(&cached_srcfiles, memalloc_ctx());
  return ok;
}


// create_pkg_api_ns creates pkg->api_ns from pkg->api
static err_t create_pkg_api_ns(memalloc_t api_ma, pkg_t* pkg) {
  nsexpr_t* ns = NULL;

  // allocate namespace type
  nstype_t* nst = (nstype_t*)ast_mknode(api_ma, sizeof(nstype_t), TYPE_NS);
  if (!nst)
    goto oom;
  nst->flags |= NF_CHECKED;
  if (!nodearray_reserve_exact(&nst->members, api_ma, pkg->api.len))
    goto oom;

  // create package namespace node
  ns = (nsexpr_t*)ast_mknode(api_ma, sizeof(nsexpr_t), EXPR_NS);
  if (!ns)
    goto oom;
  sym_t* member_names = mem_alloc(api_ma, sizeof(sym_t) * (usize)pkg->api.len).p;
  if (!member_names)
    goto oom;
  ns->flags |= NF_CHECKED | NF_PKGNS;
  ns->name = sym__;
  ns->type = (type_t*)nst;
  ns->members = pkg->api;
  ns->member_names = member_names;
  ns->pkg = pkg; // note: "pkg" field is only valid with flags&NF_PKGNS

  // populate namespace type members and member_names
  for (u32 i = 0; i < pkg->api.len; i++) {
    node_t* n = pkg->api.v[i];
    switch (n->kind) {
      case EXPR_FUN: {
        fun_t* fn = (fun_t*)n;
        member_names[i] = fn->name ? fn->name : sym__;
        nst->members.v[i] = (node_t*)fn->type;
        break;
      }
      case STMT_TYPEDEF: {
        type_t* t = ((typedef_t*)n)->type;
        if (t->kind == TYPE_STRUCT) {
          member_names[i] = ((structtype_t*)t)->name ? ((structtype_t*)t)->name : sym__;
        } else {
          assertf(t->kind == TYPE_ALIAS, "unexpected %s", nodekind_name(t->kind));
          member_names[i] = ((aliastype_t*)t)->name;
        }
        nst->members.v[i] = (node_t*)type_unknown;
        break;
      }
      default:
        safecheckf(0, "TODO %s %s", __FUNCTION__, nodekind_name(n->kind));
        member_names[i] = sym__;
        nst->members.v[i] = (node_t*)type_unknown;

    } // switch
  }

  assertnull(pkg->api_ns);
  pkg->api_ns = ns;

  // if (opt_trace_parse && c->opt_printast) {
  //   dlog("————————— AST pkg.api %s —————————", pkg->path.p);
  //   dump_ast((node_t*)ns);
  // }

  return 0;

oom:
  if (nst) {
    nodearray_dispose(&nst->members, api_ma);
    mem_freex(api_ma, MEM(nst, sizeof(*nst)));
  }
  if (ns)
    mem_freex(api_ma, MEM(ns, sizeof(*ns)));
  return ErrNoMem;
}


// load_pkg_api decodes AST from astdec and assigns it to pkg->api
static err_t load_pkg_api(memalloc_t api_ma, pkg_t* pkg, astdecoder_t* astdec) {
  node_t** nodev;
  u32 nodec;
  err_t err = astdecoder_decode_ast(astdec, &nodev, &nodec);
  if (err) {
    dlog("astdecode error: %s", err_str(err));
    return err;
  }
  // dlog("[%s \"%s\"] decoded %u node%s",
  //   __FUNCTION__, pkg->path.p, nodec, nodec == 1 ? "" : "s");

  // add declarations to pkg->api
  pkg->api.v = nodev;
  pkg->api.cap = nodec;
  pkg->api.len = nodec;

  return create_pkg_api_ns(api_ma, pkg);
}


static err_t build_dependency(compiler_t* c, memalloc_t api_ma, pkgcell_t pkgc) {
  trace_import("\"%s\" building dependency \"%s\"",
    pkgc.parent->pkg->path.p, pkgc.pkg->path.p);
  u32 pkgbuildflags = PKGBUILD_DEP;
  err_t err = build_pkg(pkgc, c, /*outfile*/"", api_ma, pkgbuildflags);
  if (err)
    dlog("error while building pkg %s: %s", pkgc.pkg->path.p, err_str(err));
  return err;
}


static bool load_dependency1(
  compiler_t* c, memalloc_t api_ma, pkgcell_t pkgc,
  const sha256_t* old_api_sha256v, err_t* errp)
{
  pkg_t* pkg = pkgc.pkg;

  // check if source files have been modified
  if (pkg->mtime == 0)
    return false;
  if (!check_pkg_src_uptodate(pkg, pkg->mtime))
    return false;

  err_t err = 0;
  bool is_uptodate = true;

  // load sub-dependencies packages (which might cause us to build them.)
  for (u32 i = 0; i < pkg->imports.len; i++) {
    pkg_t* dep = pkg->imports.v[i];
    // load last one sync to make full use of the current thread
    bool use_curr_thread = (i == pkg->imports.len - 1);
    load_dependency(c, api_ma, &pkgc, dep, use_curr_thread);
  }

  // wait for dependencies to finish loading and check their status
  for (u32 i = 0; i < pkg->imports.len; i++) {
    pkg_t* dep = pkg->imports.v[i];
    trace_import("%s: waiting for pkg(%s) to load...", __FUNCTION__, dep->path.p);
    if (( err = future_wait(&dep->loadfut) ))
      goto end;

    // if the dependency was modified earlier than the dependant, it's up to date
    if (dep->mtime <= pkg->mtime)
      continue;

    // The dependency has recently been modified (maybe we just built it.)
    // Check if its API changed
    if (memcmp(&old_api_sha256v[i], &dep->api_sha256, 32) != 0) {
      // dep API changed (or was previously unknown)
      trace_import("[%s] dep \"%s\" changed", pkg->path.p, relpath(dep->dir.p));

      // note: it's okay to stop early and not future_wait all dependencies since
      // load_dependency0 will call build_dependency when we return false, which in turn
      // will future_wait all its dependencies.
      is_uptodate = false;
      break;
    }

    trace_import("[%s] API of \"%s\" unchanged", pkg->path.p, dep->path.p);
  }

end:
  return is_uptodate && err == 0;
}


// load_dependency0
//
// 1. check if there's a valid metafile, and if so, load it, and:
//    1. parse header of metafile
//    2. compare mtime of sources to metafile; if a src is newer, we must rebuild
//
static void load_dependency0(
  compiler_t* c, memalloc_t api_ma, const pkgcell_t* parent, pkg_t* pkg)
{
  err_t err;
  str_t metafile = {0};       // path to metafile
  unixtime_t libmtime = 0;    // mtime of package library file
  const void* encdata = NULL; // contents of metafile
  struct stat metast;         // status of metafile
  astdecoder_t* astdec = NULL;
  bool did_build = false; // true if we have called build_dependency
  pkgcell_t pkgc = { .parent = parent, .pkg = pkg };
  sha256_t* imports_api_sha256v = NULL;
  u32 imports_api_sha256c = 0;

  // get library file mtime
  str_t libfile = {0};
  if (!pkg_libfile(pkg, c, &libfile)) {
    err = ErrNoMem;
    goto end;
  }
  libmtime = fs_mtime(libfile.p);
  vlog("load dependency \"%s\"", pkgc.pkg->path.p);
  str_free(libfile);

  // construct metafile path
  if (!pkg_buildfile(pkg, c, &metafile, PKG_METAFILE_NAME)) {
    err = ErrNoMem;
    goto end;
  }

  // if no libfile exist, build
  if (libmtime == 0) {
    did_build = true;
    if (( err = build_dependency(c, api_ma, pkgc) )) {
      dlog("build_dependency: %s", err_str(err));
      encdata = NULL;
      goto end;
    }
  }

  // try to open metafile in read-only mode
open_metafile:
  if UNLIKELY(( err = mmap_file_ro(metafile.p, &encdata, &metast) )) {
    // note: encdata is set to NULL when mmap_file_ro fails
    if (err != ErrNotFound) {
      elog("%s: failed to read (%s)", relpath(metafile.p), err_str(err));
      goto end;
    }

    // if this is our second attempt and the file is still not showing;
    // something is broken with build_pkg or the file system (or a race happened)
    if (did_build) {
      elog("%s: failed to build", relpath(metafile.p));
      goto end;
    }

    // build package and then try opening metafile again
    did_build = true;
    if (( err = build_dependency(c, api_ma, pkgc) )) {
      dlog("build_dependency: %s", err_str(err));
      goto end;
    }
    goto open_metafile;
  }

  // when we get here, the metafile is open for reading

  // open an AST decoder
  astdec = astdecoder_open(c, api_ma, metafile.p, encdata, metast.st_size);
  if (!astdec) {
    err = ErrNoMem;
    dlog("astdecoder_open: %s", err_str(err));
    goto end;
  }

  // decode package information; astdecoder_decode_header populates ...
  // - decpkg.{path,dir,root} (+ verifies they match prior input values of pkg)
  // - decpkg.files via pkg_add_srcfile
  // - length of decpkg.imports
  u32 importcount;
  if (( err = astdecoder_decode_header(astdec, pkg, &importcount) )) {
    dlog("astdecoder_decode_header: %s", err_str(err));
  } else {
    // update pkg->mtime to mtime of metafile
    pkg->mtime = MIN(libmtime, unixtime_of_stat_mtime(&metast));

    // allocate memory for memorized API checksums
    if (importcount > 0) {
      void* p = mem_resizev(
        c->ma, imports_api_sha256v, imports_api_sha256c, importcount, sizeof(sha256_t));
      if (!p) {
        dlog("mem_resizev(%p,%u,%u) OOM",
          imports_api_sha256v, imports_api_sha256c, importcount);
        err = ErrNoMem;
        goto end;
      }
      imports_api_sha256v = p;
    }
    imports_api_sha256c = importcount;

    // decode imports
    if (( err = astdecoder_decode_imports(astdec, pkg, imports_api_sha256v)) )
      dlog("astdecoder_decode_imports: %s", err_str(err));
  }

  // check for decoding errors
  if (err) {
    if (did_build)
      goto end;
    // try building; maybe the metafile is b0rked
    pkg->mtime = 0;
    err = 0;
  }

  // unless we just built the package, check source files and load sub-dependencies
  if (!did_build && pkg->mtime > 0 &&
      !load_dependency1(c, api_ma, pkgc, imports_api_sha256v, &err))
  {
    if (err)
      goto end;
    // source files have been modified
    // must clear any srcfiles & imports loaded from (possibly stale) metafile
    pkg->srcfiles.len = 0;
    pkg->imports.len = 0;

    // at least one source file has been modified since metafile was modified
    did_build = true;
    if (( err = build_dependency(c, api_ma, pkgc) ))
      goto end;

    // close old metafile and associated resources
    astdecoder_close(astdec); astdec = NULL;
    mmap_unmap((void*)encdata, metast.st_size); encdata = NULL;

    // open the new metafile
    goto open_metafile;
  }

  // When we get here, pkg is loaded & up-to-date.
  // We now need to load the package's API.
  err = load_pkg_api(api_ma, pkg, astdec);

end:

  future_finalize(&pkg->loadfut, err);
  if (err) {
    trace_import("loaded package \"%s\" error: %s", pkg->path.p, err_str(err));
  } else {
    trace_import("loaded package \"%s\" OK", pkg->path.p);
  }

  if (astdec)
    astdecoder_close(astdec);
  if (encdata)
    mmap_unmap((void*)encdata, metast.st_size);
  if (imports_api_sha256v)
    mem_freetv(c->ma, imports_api_sha256v, imports_api_sha256c);
  str_free(metafile);
}


static void load_dependency(
  compiler_t* c, memalloc_t api_ma, const pkgcell_t* parent, pkg_t* pkg, bool sync)
{
  if (!future_acquire(&pkg->loadfut)) {
    // already loaded or it's currently in the process of being loaded
    return;
  }

  // if COMAXPROC is set to 1 or there is only one CPU available, don't use threads
  if (comaxproc == 1 || sync) {
    load_dependency0(c, api_ma, parent, pkg);
    return;
  }

  UNUSED err_t err;
  err = threadpool_submit(load_dependency0, c, api_ma, parent, pkg);
  // threadpool_submit only fails if we pass more than THREADPOOL_MAX_ARGS, which is
  // checked at compile time when using threadpool_submit instead of threadpool_submitv.
  assertf(!err, "threadpool_submit: %s", err_str(err));
}


static bool report_import_cycle(pkgbuild_t* pb, const pkg_t* pkg) {
  elog("import cycle not allowed; import stack:");
  pkgcell_t pkgc = pb->pkgc;
  elog("  %s\t(%s)", pkg->path.p, pkg->dir.p);
  for (;;) {
    elog("  %s\t(%s)", pkgc.pkg->path.p, pkgc.pkg->dir.p);
    if (pkgc.parent == NULL /*|| pkg == pkgc.pkg*/)
      break;
    pkgc = *pkgc.parent;
  }
  return false;
}


static bool check_import_cycle(pkgbuild_t* pb, const pkg_t* pkg) {
  pkgcell_t pkgc = pb->pkgc;
  for (;;) {
    if UNLIKELY(pkg == pkgc.pkg)
      return report_import_cycle(pb, pkg);
    if (pkgc.parent == NULL)
      break;
    pkgc = *pkgc.parent;
  }
  return true;
}


UNUSED static void trace_dependencies(const pkg_t* pkg, int indent) {
  for (u32 i = 0; i < pkg->imports.len; i++) {
    const pkg_t* dep = pkg->imports.v[i];
    trace_import_indented(indent*2, "%s", dep->path.p);
    trace_dependencies(dep, indent + 1);
  }
}


static err_t get_runtime_pkg(pkgbuild_t* pb, pkg_t** rt_pkg) {
  // we cache the std/runtime package at compiler_t.stdruntime_pkg
  rwmutex_rlock(&pb->c->pkgindex_mu);
  *rt_pkg = pb->c->stdruntime_pkg;
  rwmutex_runlock(&pb->c->pkgindex_mu);
  if (*rt_pkg) // found in cache
    return 0;

  err_t err;
  slice_t rt_pkgpath = slice_cstr("std/runtime");
  str_t rt_pkgdir = str_makelen(rt_pkgpath.chars, rt_pkgpath.len);
  usize rt_rootlen;

  // Resolve package
  // This will fail if it's not found on disk
  if (( err = import_resolve_fspath(&rt_pkgdir, &rt_rootlen) ))
    goto end;

  // check sanity of import_resolve_fspath
  assertf(
    strcmp(rt_pkgpath.chars, rt_pkgdir.p + rt_rootlen + 1) == 0,
    "import_resolve_fspath returned rootlen=%zu, dir='%s'",
    rt_rootlen, rt_pkgdir.p);

  // intern package in pkgindex
  err = pkgindex_intern(
    pb->c, str_slice(rt_pkgdir), rt_pkgpath, /*api_sha256*/NULL, rt_pkg);

end:
  str_free(rt_pkgdir);
  rwmutex_lock(&pb->c->pkgindex_mu);
  // note: no race because of pkgindex_intern
  pb->c->stdruntime_pkg = *rt_pkg;
  rwmutex_unlock(&pb->c->pkgindex_mu);
  return err;
}


err_t pkgbuild_import(pkgbuild_t* pb) {
  pkg_t* pkg = pb->pkgc.pkg;
  err_t err;

  assert(pkg->imports.len == 0);

  // add "std/runtime" dependency (for top-level packages only)
  if ((pb->flags & PKGBUILD_DEP) == 0 && pb->c->opt_nostdruntime == false) {
    pkg_t* rt_pkg;
    if (( err = get_runtime_pkg(pb, &rt_pkg) ))
      return err;
    // note: "rt_pkg!=pkg" guards std/runtime from importing itself
    if (rt_pkg != pkg && !ptrarray_push(&pkg->imports, pb->c->ma, rt_pkg)) {
      err = ErrNoMem;
      return err;
    }
  }

  // import_pkgs
  // 1. finds all unique imports across units
  // 2. resolves each imported package
  if (( err = import_pkgs(pb->c, pkg, pb->unitv, pb->unitc) ))
    return err;

  // stop now if no packages are imported
  if (pkg->imports.len == 0)
    return 0;

  // trim excess space of imports array since we'll be keeping it around
  ptrarray_shrinkwrap(&pkg->imports, pb->c->ma);

  // at this point all packages at pkg->imports ...
  // - are verified to exist (have a valid pkg->path, pkg->dir & pkg->root)
  // - may or may not be ready for use (may need to be built before it can be used)

  // trace imported packages
  #ifdef DEBUG
  if (opt_trace_import) {
    trace_import("\"%s\" importing %u packages:", pkg->path.p, pkg->imports.len);
    for (u32 i = 0; i < pkg->imports.len; i++) {
      pkg_t* dep = pkg->imports.v[i];
      trace_import("  %s (root %s)", dep->path.p, dep->root.p);
    }
  }
  #endif

  // check for early import cycles
  for (u32 i = 0; i < pkg->imports.len; i++) {
    pkg_t* dep = pkg->imports.v[i];
    if (!check_import_cycle(pb, dep))
      return ErrCanceled;
  }

  // load imported packages (which might cause us to build them.)
  for (u32 i = 0; i < pkg->imports.len; i++) {
    pkg_t* dep = pkg->imports.v[i];

    // load last one sync to make full use of the current thread
    bool use_curr_thread = (i == pkg->imports.len - 1);
    load_dependency(pb->c, pb->api_ma, &pb->pkgc, dep, use_curr_thread);
  }

  // wait for imported packages to load
  for (u32 i = 0; i < pkg->imports.len; i++) {
    pkg_t* dep = pkg->imports.v[i];
    trace_import("%s: waiting for pkg(%s) to load...", __FUNCTION__, dep->path.p);
    // note: it's okay to stop early and not future_wait all dependencies
    if (( err = future_wait(&dep->loadfut) ))
      break;
  }

  #ifdef DEBUG
  if (opt_trace_import) {
    trace_import("dependency tree for package \"%s\":", pkg->path.p);
    trace_dependencies(pkg, 1);
  }
  #endif

  return err;
}


static err_t report_bad_mainfun(pkgbuild_t* pb, const fun_t* fn) {
  // There's a "main" function but it doesn't qualify for being THE main function.
  // Note that we know that the issue is missing "pub" qualifier, since typecheck
  // has already verified that a "pub fun main(..." has the correct signature.
  const funtype_t* ft = assertnotnull((funtype_t*)fn->type);

  if (ft->params.len == 0 && ft->result == type_void) {
    report_diag(pb->c, ast_origin(&pb->c->locmap, (node_t*)fn), DIAG_ERR,
      "program's main function is not public");
    report_diag(pb->c, origin_make(&pb->c->locmap, fn->loc), DIAG_HELP,
      "mark function as `pub` (or build with --no-main flag)");
  } else {
    report_diag(pb->c, ast_origin(&pb->c->locmap, (node_t*)fn), DIAG_ERR,
      "invalid signature of program's main function");
    report_diag(pb->c, origin_make(&pb->c->locmap, fn->loc), DIAG_HELP,
      "change signature to `pub fun main()` (or build with --no-main flag)");
  }

  return ErrCanceled;
}


err_t pkgbuild_typecheck(pkgbuild_t* pb) {
  err_t err;
  compiler_t* c = pb->c;

  pkgbuild_begintask(pb, "typecheck");

  if (pb->unitc == 0)
    return 0;

  dlog_if(opt_trace_typecheck, "————————— typecheck —————————");

  // make sure there are no parse errors
  if (compiler_errcount(c) > 0) {
    dlog("%s called with pre-existing parse errors", __FUNCTION__);
    return ErrCanceled;
  }

  // typecheck
  if (( err = typecheck(c, pb->ast_ma, pb->pkgc.pkg, pb->unitv, pb->unitc) )) {
    dlog("typecheck: %s", err_str(err));
    return err;
  }
  if (compiler_errcount(c) > 0) {
    dlog("typecheck: %u diagnostic errors", compiler_errcount(c));
    if (!opt_trace_parse && c->opt_printast)
      dump_pkg_ast(pb->pkgc.pkg, pb->unitv, pb->unitc);
    return ErrCanceled;
  }

  // trace & dlog
  if (opt_trace_typecheck && c->opt_printast) {
    dlog("————————— AST after typecheck —————————");
    dump_pkg_ast(pb->pkgc.pkg, pb->unitv, pb->unitc);
  }

  // check for cyclic types
  if (( err = check_typedeps(c, pb->unitv, pb->unitc) )) {
    dlog("check_typedeps: %s", err_str(err));
    return err;
  }
  if (compiler_errcount(c) > 0) {
    dlog("check_typedeps: %u diagnostic errors", compiler_errcount(c));
    return ErrCanceled;
  }

  // build IR -- performs ownership analysis; updates "drops" lists in AST
  dlog_if(opt_trace_ir, "————————— IR —————————");
  if (( err = iranalyze(c, pb->ast_ma, pb->pkgc.pkg, pb->unitv, pb->unitc) )) {
    dlog("iranalyze: %s", err_str(err));
    return err;
  }
  if (compiler_errcount(c) > 0) {
    dlog("iranalyze: %u diagnostic errors", compiler_errcount(c));
    return ErrCanceled;
  }

  // trace & dlog
  if (opt_trace_ir && c->opt_printast) {
    dlog("————————— AST after IR —————————");
    dump_pkg_ast(pb->pkgc.pkg, pb->unitv, pb->unitc);
  }

  // print AST, if requested
  if (c->opt_printast) {
    if (opt_trace_parse || opt_trace_typecheck || opt_trace_ir) {
      // we have printed the AST at various stages already,
      // so let's print a header to make it easier to distinguish what is what
      dlog("————————— AST after analyze —————————");
    }
    dump_pkg_ast(pb->pkgc.pkg, pb->unitv, pb->unitc);
  }

  return 0;
}


err_t pkgbuild_setinfo(pkgbuild_t* pb) {
  // create public namespace for package, at pkg->api
  // first, count declarations so we can allocate an array of just the right size
  u32 nmembers = 0;
  for (u32 i = 0; i < pb->unitc; i++) {
    nodearray_t decls = pb->unitv[i]->children;
    for (u32 i = 0; i < decls.len; i++)
      nmembers += (u32)!!(decls.v[i]->flags & NF_VIS_PUB);
  }
  // create & populate api array
  pkg_t* pkg = pb->pkgc.pkg;
  assert(pkg->api.len == 0);
  if (!nodearray_reserve_exact(&pkg->api, pb->ast_ma, nmembers))
    return ErrNoMem;
  for (u32 i = 0; i < pb->unitc; i++) {
    nodearray_t decls = pb->unitv[i]->children;
    for (u32 i = 0; i < decls.len; i++) {
      // skip non-public statements
      if ((decls.v[i]->flags & NF_VIS_PUB) == 0)
        continue;

      // skip public function _declarations_
      if (decls.v[i]->kind == EXPR_FUN && ((fun_t*)decls.v[i])->body == NULL)
        continue;

      pkg->api.v[pkg->api.len++] = decls.v[i];
    }
  }

  // Determine if we are building an executable or a library.
  // (Note that non-top-level packages (i.e dependencies) are flagged PKGBUILD_DEP)
  assertf((pb->flags & PKGBUILD_EXE) == 0, "PKGBUILD_EXE flag is set");
  if ((pb->flags & PKGBUILD_DEP) == 0 && pb->c->opt_nomain == false) {
    // check if there's a main function
    const fun_t** mainfunp = (const fun_t**)map_lookup_ptr(&pkg->defs, sym_main);
    if (mainfunp && (*mainfunp)->kind == EXPR_FUN) {
      if UNLIKELY(!ast_is_main_fun(*mainfunp))
        return report_bad_mainfun(pb, *mainfunp);
      // we have a proper "main" function
      pb->flags |= PKGBUILD_EXE;
    }
  }

  return 0;
}


static err_t pkgbuild_cgen_pub_api(pkgbuild_t* pb) {
  str_t pubhfile = {0};
  if UNLIKELY(!pkg_buildfile(pb->pkgc.pkg, pb->c, &pubhfile, PKG_APIHFILE_NAME))
    return ErrNoMem;

  if (pb->c->opt_verbose)
    pkgbuild_begintask(pb, "cgen %s", relpath(pubhfile.p));

  err_t err = cgen_pkgapi(&pb->cgen, (unit_t**)pb->unitv, pb->unitc, &pb->pkgapi);
  if (err) {
    dlog("cgen_pkgapi: %s", err_str(err));
    goto end;
  }

  if (opt_trace_cgen) {
    fprintf(stderr, "—————————— cgen API %s ——————————\n", relpath(pubhfile.p));
    fwrite(pb->pkgapi.pub_header.chars, pb->pkgapi.pub_header.len, 1, stderr);
    fputs("\n——————————————————————————————————\n", stderr);
  }

  // compute SHA-256 sum of public API
  sha256_data(
    &pb->pkgc.pkg->api_sha256,
    pb->pkgapi.pub_header.p, pb->pkgapi.pub_header.len);

  err = fs_writefile_mkdirs(pubhfile.p, 0660, pb->pkgapi.pub_header);

end:
  str_free(pubhfile);
  return err;
}


err_t pkgbuild_cgen_pub(pkgbuild_t* pb) {
  err_t err;
  compiler_t* c = pb->c;

  dlog_if(opt_trace_cgen, "————————— cgen —————————");
  assert(pb->pkgc.pkg->srcfiles.len > 0);

  // create C code generator
  u32 cgen_flags = 0;
  if ((pb->flags & PKGBUILD_EXE)) {
    assert(c->opt_nomain == false); // checked before setting PKGBUILD_EXE
    cgen_flags |= CGEN_EXE;
  }
  if (pb->c->buildmode == BUILDMODE_DEBUG) {
    // Include `#line N "source.co"` in C code generated for debug builds.
    // This aids in debugging issues with cgen, so it's really a tool for
    // developing Compis itself, not user programs.
    // But Compis is still largely untested so "users" will find bugs.
    cgen_flags |= CGEN_SRCINFO;
  }
  if (!cgen_init(&pb->cgen, c, pb->pkgc.pkg, c->ma, cgen_flags)) {
    dlog("cgen_init: %s", err_str(ErrNoMem));
    return ErrNoMem;
  }

  // create output dir and initialize cfiles & ofiles arrays
  if (( err = prepare_builddir(pb) ))
    return err;

  // generate package C header
  if (( err = pkgbuild_cgen_pub_api(pb) ))
    return err;

  return err;
}


err_t pkgbuild_cgen_pkg(pkgbuild_t* pb) {
  err_t err = 0;

  // generate one C file for each unit
  for (u32 i = 0; i < pb->unitc; i++) {
    unit_t* unit = pb->unitv[i];
    const char* cfile = cfile_of_unit(pb, unit);

    if (pb->c->opt_verbose)
      pkgbuild_begintask(pb, "cgen %s", relpath(cfile));

    if (( err = cgen_unit_impl(&pb->cgen, unit, &pb->pkgapi) ))
      break;

    if (opt_trace_cgen) {
      fprintf(stderr, "—————————— cgen %s ——————————\n", relpath(cfile));
      fwrite(pb->cgen.outbuf.p, pb->cgen.outbuf.len, 1, stderr);
      fputs("\n——————————————————————————————————\n", stderr);
    }

    if (( err = fs_writefile_mkdirs(cfile, 0660, buf_slice(pb->cgen.outbuf)) ))
      break;
  }

  return err;
}


err_t pkgbuild_metagen(pkgbuild_t* pb) {
  err_t err = 0;
  pkg_t* pkg = pb->pkgc.pkg;

  str_t filename = {0};
  if (!pkg_buildfile(pkg, pb->c, &filename, PKG_METAFILE_NAME))
    return ErrNoMem;

  if (pb->c->opt_verbose)
    pkgbuild_begintask(pb, "metagen %s", relpath(filename.p));

  buf_t outbuf = buf_make(pb->c->ma);

  // create AST encoder
  astencoder_t* astenc = astencoder_create(pb->c);
  if (!astenc) {
    err = ErrNoMem;
    goto end;
  }

  // encoders can be reused, so we need to tell it to start an encoding session
  astencoder_begin(astenc, pkg);

  // add top-level declarations from pkg->api
  for (u32 i = 0; i < pkg->api.len && err == 0; i++) {
    err = astencoder_add_ast(astenc, pkg->api.v[i], ASTENCODER_PUB_API);
    if (err)
      dlog("astencoder_add_ast: %s", err_str(err));
  }

  // Register all source files.
  // This is needed since, even though astencoder_add_ast implicitly registers source
  // files for us, it only does so for nodes which are part of the public package API.
  // I.e. if a source file does not contain any public definitions, it will not be
  // automatically registered.
  // Note that it does not matter if we call astencoder_add_srcfile before or after
  // calling astencoder_add_ast, as source files are ordered by the encoder, so the
  // results are the same no matter the order we call these functions.
  for (u32 i = 0; i < pkg->srcfiles.len && err == 0; i++) {
    err = astencoder_add_srcfile(astenc, pkg->srcfiles.v[i]);
    if (err) {
      dlog("astencoder_add_srcfile(%s): %s",
        ((srcfile_t*)pkg->srcfiles.v[i])->name.p, err_str(err));
    }
  }

  // finalize
  if (!err)
    err = astencoder_encode(astenc, &outbuf);
  astencoder_free(astenc);
  if (err)
    goto end;

  // write to file
  err = fs_writefile_mkdirs(filename.p, 0644, buf_slice(outbuf));

end:
  str_free(filename);
  buf_dispose(&outbuf);
  return err;
}


err_t pkgbuild_begin_late_compilation(pkgbuild_t* pb) {
  pkg_t* pkg = pb->pkgc.pkg;

  if (pkg->srcfiles.len == 0)
    return 0;

  err_t err = 0;
  assertf(pb->ofiles.len > 0, "prepare_builddir not called");

  for (u32 i = 0; i < pkg->srcfiles.len && err == 0; i++) {
    srcfile_t* srcfile = pkg->srcfiles.v[i];
    if (srcfile->type != FILE_CO)
      continue;
    const char* cfile = cfile_of_srcfile_id(pb, i);
    const char* ofile = ofile_of_srcfile_id(pb, i);
    pkgbuild_begintask(pb, "compile %s",
      pb->c->opt_verbose ? relpath(cfile) : srcfile->name.p);
    err = compile_c_source(pb, &pb->promisev[i], cfile, ofile);
    if (err)
      dlog("compile_c_source: %s", err_str(err));
  }

  return err;
}


err_t pkgbuild_await_compilation(pkgbuild_t* pb) {
  err_t err = 0;
  for (u32 i = 0; i < pb->pkgc.pkg->srcfiles.len; i++) {
    err_t err1 = promise_await(&pb->promisev[i]);
    if (!err)
      err = err1;
  }
  return err;
}


static bool deplist_add_deps_of(ptrarray_t* deplist, memalloc_t ma, const pkg_t* pkg) {
  for (u32 i = 0; i < pkg->imports.len; i++) {
    const pkg_t* dep = pkg->imports.v[i];
    if (!ptrarray_sortedset_addptr(deplist, ma, dep, NULL))
      return false;
    if (!deplist_add_deps_of(deplist, ma, dep))
      return false;
  }
  return true;
}


static err_t link_exe(pkgbuild_t* pb, const char* outfile) {
  compiler_t* c = pb->c;
  err_t err = 0;
  str_t lto_cachedir = {0};
  ptrarray_t deplist = {0}; // pkg_t*[]
  ptrarray_t libfiles = {0}; // const char*[]

  // TODO: -Llibdir
  // char libflag[PATH_MAX];
  // snprintf(libflag, sizeof(libflag), "-L%s", c->libdir);

  // build list of (unique) dependencies
  if (!deplist_add_deps_of(&deplist, pb->c->ma, pb->pkgc.pkg)) {
    err = ErrNoMem;
    goto end;
  }

  // build list of libfiles for each dependency
  if (!ptrarray_reserve_exact(&libfiles, pb->c->ma, deplist.len)) {
    err = ErrNoMem;
    goto end;
  }
  for (u32 i = 0; i < deplist.len; i++) {
    pkg_t* dep = deplist.v[i];
    str_t libfile = {0};
    if (!pkg_libfile(dep, pb->c, &libfile)) {
      err = ErrNoMem;
      goto end;
    }
    libfiles.v[libfiles.len++] = libfile.p;
  }

  CoLLVMLink link = {
    .target_triple = c->target.triple,
    .outfile = outfile,
    .infilev = (const char*const*)strlist_array(&pb->ofiles),
    .infilec = pb->ofiles.len,
    .libfilev = (const char**)libfiles.v,
    .libfilec = libfiles.len,
    .sysroot = c->sysroot,
    .print_lld_args = coverbose > 1,
    .lto_level = 0,
    .lto_cachedir = "",
  };

  // configure LTO
  if (c->buildmode == BUILDMODE_OPT && !target_is_riscv(&c->target)) {
    if (!pkg_buildfile(pb->pkgc.pkg, pb->c, &lto_cachedir, "llvm")) {
      err = ErrNoMem;
      goto end;
    }
    link.lto_level = 2;
    link.lto_cachedir = lto_cachedir.p;
  }

  err = llvm_link(&link);
  if (err)
    dlog("llvm_link: %s", err_str(err));

end:
  for (u32 i = 0; i < libfiles.len; i++)
    str_free( ((str_t){ libfiles.v[i], strlen((char*)libfiles.v[i]), 0 }) );
  ptrarray_dispose(&libfiles, pb->c->ma);
  ptrarray_dispose(&deplist, pb->c->ma);
  str_free(lto_cachedir);
  return err;
}


static err_t link_lib_archive(pkgbuild_t* pb, const char* outfile) {
  compiler_t* c = pb->c;
  err_t err = 0;

  CoLLVMArchiveKind ar_kind;
  if (c->target.sys == SYS_none) {
    ar_kind = llvm_sys_archive_kind(target_default()->sys);
  } else {
    ar_kind = llvm_sys_archive_kind(c->target.sys);
  }

  const char*const* ofilev = (const char*const*)strlist_array(&pb->ofiles);
  u32 ofilec = pb->ofiles.len;

  char* errmsg = "?";
  err = llvm_write_archive(ar_kind, outfile, ofilev, ofilec, &errmsg);

  if UNLIKELY(err) {
    elog("llvm_write_archive: (err=%s) %s", err_str(err), errmsg);
    if (err == ErrNotFound) {
      for (u32 i = 0; i < ofilec; i++) {
        if (!fs_isfile(ofilev[i]))
          elog("%s: file not found", ofilev[i]);
      }
    }
    LLVMDisposeMessage(errmsg);
  }

  return err;
}


err_t pkgbuild_link(pkgbuild_t* pb, const char* outfile) {
  if (pb->flags & PKGBUILD_NOLINK) {
    dlog("pkgbuild_link: skipped because PKGBUILD_NOLINK flag is set");
    return 0;
  }

  assert_promises_completed(pb);

  pkg_t* pkg = pb->pkgc.pkg;
  str_t outfile_str = {0};
  err_t err = 0;

  // if no outfile is given, use the default one
  if (*outfile == 0) {
    bool ok;
    if (pb->flags & PKGBUILD_EXE) {
      ok = pkg_exefile(pkg, pb->c, &outfile_str);
    } else {
      ok = pkg_libfile(pkg, pb->c, &outfile_str);
    }
    if (!ok) {
      str_free(outfile_str);
      return ErrNoMem;
    }
    outfile = outfile_str.p;
  }

  pkgbuild_begintask(pb, "link %s", relpath(outfile));

  char* dir = path_dir_alloca(outfile);
  if (( err = fs_mkdirs(dir, 0755, FS_VERBOSE) ))
    return err;

  if (pb->flags & PKGBUILD_EXE) {
    err = link_exe(pb, outfile);
  } else {
    err = link_lib_archive(pb, outfile);
  }

  bgtask_end(pb->bgt, "%s",
    (pb->flags & PKGBUILD_NOLINK) ? "(compile only)" :
    relpath(outfile));

  str_free(outfile_str);
  return err;
}


static err_t build_pkg(
  pkgcell_t pkgc, compiler_t* c, const char* outfile,
  memalloc_t api_ma, u32 pkgbuild_flags)
{
  err_t err;
  bool did_await_compilation = false;

  if UNLIKELY(compiler_errcount(c) > 0) {
    dlog("%s failing immediately (compiler has encountered errors)", __FUNCTION__);
    return ErrCanceled;
  }

  vlog("building package \"%s\" (%s)", pkgc.pkg->path.p, pkgc.pkg->dir.p);

  // create pkgbuild_t struct
  pkgbuild_t* pb = mem_alloct(c->ma, pkgbuild_t);
  if (!pb)
    return ErrNoMem;
  if UNLIKELY(( err = pkgbuild_init(pb, pkgc, c, api_ma, pkgbuild_flags) )) {
    mem_freex(c->ma, MEM(pb, sizeof(pkgbuild_t)));
    return err;
  }

  #define DO_STEP(fn, args...) \
    if (( err = fn(pb, ##args) )) { \
      dlog("%s: %s", #fn, err_str(err)); \
      goto end; \
    }

  // locate source files
  DO_STEP(pkgbuild_locate_sources);

  // begin compilation of C source files
  DO_STEP(pkgbuild_begin_early_compilation);

  // parse source files
  DO_STEP(pkgbuild_parse);

  // resolve and import dependencies
  DO_STEP(pkgbuild_import);

  // typecheck package
  DO_STEP(pkgbuild_typecheck);

  // set package info like pkg->api and pb->flags&PKGBUILD_EXE
  DO_STEP(pkgbuild_setinfo);

  // generate public C API
  DO_STEP(pkgbuild_cgen_pub);

  // generate package metadata (can run in parallel to the rest of these tasks)
  DO_STEP(pkgbuild_metagen);

  // generate package C code
  DO_STEP(pkgbuild_cgen_pkg);

  // begin compilation of C source files generated from compis sources
  DO_STEP(pkgbuild_begin_late_compilation);

  // wait for compilation tasks to finish
  did_await_compilation = true;
  DO_STEP(pkgbuild_await_compilation);

  // link exe or library (does nothing if PKGBUILD_NOLINK flag is set)
  DO_STEP(pkgbuild_link, outfile);

end:
  if (!did_await_compilation)
    pkgbuild_await_compilation(pb);
  if ((pkgbuild_flags & PKGBUILD_NOCLEANUP) == 0) {
    pkgbuild_dispose(pb);
    mem_freet(c->ma, pb);
  }
  #undef DO_STEP
  return err;
}


err_t build_toplevel_pkg(
  pkg_t* pkg, compiler_t* c, const char* outfile, u32 pkgbuild_flags)
{
  assert((pkgbuild_flags & PKGBUILD_DEP) == 0);

  // create AST allocator for APIs, AST that needs to outlive any one package build
  memalloc_t api_ma = memalloc_bump2(/*slabsize*/0, /*flags*/0);
  if (api_ma == memalloc_null()) {
    dlog("OOM: memalloc_bump_in_zeroed");
    return ErrNoMem;
  }

  err_t err = build_pkg((pkgcell_t){NULL,pkg}, c, outfile, api_ma, pkgbuild_flags);

  if ((pkgbuild_flags & PKGBUILD_NOCLEANUP) == 0)
    memalloc_bump2_dispose(api_ma);

  return err;
}
