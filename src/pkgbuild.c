// SPDX-License-Identifier: Apache-2.0
#include "colib.h"
#include "pkgbuild.h"
#include "path.h"
#include "metagen.h"
#include "llvm/llvm.h"


err_t pkgbuild_init(pkgbuild_t* pb, pkg_t* pkg, compiler_t* c, u32 flags) {
  memset(pb, 0, sizeof(*pb));
  pb->pkg = pkg;
  pb->c = c;
  pb->flags = flags;

  pkg->defs.parent = &c->builtins;

  // configure a bgtask for communicating status to the user
  int taskflags = (c->opt_verbose > 0) ? BGTASK_NOFANCY : 0;
  u32 tasklen = 1; // typecheck
  tasklen += (u32)!!c->opt_verbose; // cgen "api header"
  tasklen += (u32)!(flags & PKGBUILD_NOLINK); // link
  pb->bgt = bgtask_open(c->ma, pkg->path.p, tasklen, taskflags);
  // note: bgtask_open currently panics on OOM; change that, make it return NULL

  // create AST allocator
  pb->ast_ma = memalloc_bump_in_zeroed(c->ma, 1024*1024*8lu, /*flags*/0);
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
  if (pb->promisev) for (u32 i = 0; i < pb->pkg->files.len; i++)
    assertf(pb->promisev[i].await == NULL, "promisev[%u] was not awaited", i);
}


void pkgbuild_dispose(pkgbuild_t* pb) {
  // srcfiles may have been opened if diagnostics were reported during
  // typecheck or cgen, so let's make sure they are all closed
  for (u32 i = 0; i < pb->pkg->files.len; i++)
    srcfile_close(&pb->pkg->files.v[i]);

  bgtask_close(pb->bgt);
  memalloc_bump_in_dispose(pb->ast_ma);
  strlist_dispose(&pb->cfiles);
  strlist_dispose(&pb->ofiles);
  if (pb->promisev) {
    assert_promises_completed(pb);
    mem_freetv(pb->c->ma, pb->promisev, (usize)pb->pkg->files.len);
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
  err_t err = node_repr(&buf, ast);
  if (!err) {
    fwrite(buf.chars, buf.len, 1, stderr);
    fputc('\n', stderr);
  }
  buf_dispose(&buf);
  return err;
}


static err_t dump_pkg_ast(const pkg_t* pkg, unit_t*const* unitv, u32 unitc) {
  buf_t buf = buf_make(memalloc_ctx());
  err_t err = ast_fmt_pkg(&buf, pkg, (const unit_t*const*)unitv, unitc);
  if (!err) {
    fwrite(buf.chars, buf.len, 1, stderr);
    fputc('\n', stderr);
  }
  buf_dispose(&buf);
  return err;
}


static void build_ofiles_and_cfiles(pkgbuild_t* pb, str_t builddir) {
  str_t s = {0};

  for (u32 i = 0; i < pb->pkg->files.len; i++) {
    // {builddir}/{srcfile}.o  (note that builddir includes pkgname)
    srcfile_t* srcfile = &pb->pkg->files.v[i];
    assert(srcfile->id == i);

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
  str_t builddir = pkg_builddir(pb->pkg, pb->c);
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
  assert(pb->cfiles.len == pb->pkg->files.len);
  assertnotnull(unit->srcfile);
  return cfile_of_srcfile_id(pb, unit->srcfile->id);
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
  const char* wdir = pb->pkg->dir.p;

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
  pkg_t* pkg = pb->pkg;

  if (pkg->files.len == 0)
    pkg_find_files(pkg);

  if (pkg->files.len == 0) {
    elog("[%s] no source files in %s", pkg->path.p, relpath(pkg->dir.p));
    return ErrNotFound;
  }

  // count number of compis source files
  u32 ncosrc = 0;
  for (u32 i = 0; i < pkg->files.len; i++)
    ncosrc += (u32)(pkg->files.v[i].type == FILE_CO);

  // update bgtask
  pb->bgt->ntotal += pkg->files.len; // "parse foo.co"
  pb->bgt->ntotal += ncosrc;         // "compile foo.co"
  if (pb->c->opt_verbose)
    pb->bgt->ntotal += ncosrc;       // "cgen foo.co"

  // allocate promise array
  if (pb->promisev) {
    assert_promises_completed(pb);
    mem_freetv(pb->c->ma, pb->promisev, (usize)pkg->files.len);
  }
  pb->promisev = mem_alloctv(pb->c->ma, promise_t, (usize)pkg->files.len);
  if UNLIKELY(!pb->promisev) {
    pkgbuild_dispose(pb);
    return ErrNoMem;
  }

  return 0;
}


err_t pkgbuild_begin_early_compilation(pkgbuild_t* pb) {
  pkg_t* pkg = pb->pkg;

  // find first C srcfile or bail out if there are no C sources in the package
  u32 i = 0;
  for (; i < pkg->files.len && pkg->files.v[i].type != FILE_C; i++) {}
  if (i == pkg->files.len)
    return 0; // no C sources

  err_t err = 0;

  // create output dir and build cfiles & ofiles
  if (( err = prepare_builddir(pb) ))
    return err;

  for (; i < pkg->files.len && err == 0; i++) {
    srcfile_t* srcfile = &pkg->files.v[i];
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


static err_t parse_co_file(
  pkg_t* pkg, compiler_t* c, srcfile_t* srcfile, memalloc_t ast_ma, unit_t** result)
{
  err_t err;
  if (( err = srcfile_open(srcfile) )) {
    elog("%s: %s", srcfile->name.p, err_str(err));
    return err;
  }

  parser_t parser;
  if (!parser_init(&parser, c)) {
    dlog("parser_init failed");
    err = ErrNoMem;
    goto end;
  }

  dlog_if(opt_trace_parse, "————————— parse %s —————————",
    relpath(path_join(pkg->dir.p, srcfile->name.p).p)); // leaking memory :-/
  err = parser_parse(&parser, ast_ma, srcfile, result);
  if (!err && parser_errcount(&parser) > 0) {
    dlog("syntax errors");
    err = ErrCanceled;
  }

  if (opt_trace_parse && c->opt_printast) {
    dlog("————————— AST after parse —————————");
    dump_ast((node_t*)assertnotnull(*result));
  }

end:
  srcfile_close(srcfile);
  return err;
}


static err_t pkgbuild_parse(pkgbuild_t* pb) {
  compiler_t* c = pb->c;
  pkg_t* pkg = pb->pkg;

  // count number of compis source files
  u32 ncosrc = 0;
  for (u32 i = 0; i < pkg->files.len; i++)
    ncosrc += (u32)(pkg->files.v[i].type == FILE_CO);

  // allocate unit array
  unit_t** unitv = mem_alloctv(pb->ast_ma, unit_t*, (usize)ncosrc);
  if (!unitv) {
    dlog("out of memory");
    return ErrNoMem;
  }
  pb->unitv = unitv;
  pb->unitc = ncosrc;

  // parse each file
  err_t err = 0;
  for (u32 i = 0; i < pkg->files.len && err == 0; i++) {
    srcfile_t* srcfile = &pkg->files.v[i];

    if (srcfile->type != FILE_CO) {
      assertf(srcfile->type == FILE_C, "%s: unrecognized file type", srcfile->name.p);
      continue;
    }

    pkgbuild_begintask(pb, "parse %s", relpath(srcfile->name.p));
    err = parse_co_file(pkg, c, srcfile, pb->ast_ma, unitv);
    unitv++;
  }

  #if DEBUG
  for (u32 i = 0; i < pb->unitc && err == 0; i++)
    assertnotnull(pb->unitv[i]);
  #endif

  return err;
}


err_t pkgbuild_import(pkgbuild_t* pb) {
  assert(pb->pkg->imports.len == 0);
  err_t err = import_find_pkgs(
    pb->c, pb->pkg, (const unit_t*const*)pb->unitv, pb->unitc, &pb->pkg->imports);
  if (err)
    return err;

  // return if no packages are imported
  if (pb->pkg->imports.len == 0)
    return 0;

  dlog("imported packages:");
  for (u32 i = 0; i < pb->pkg->imports.len; i++)
    dlog("  %s", ((pkg_t*)pb->pkg->imports.v[i])->dir.p);

  for (u32 i = 0; i < pb->pkg->imports.len; i++) {
    pkg_t* pkg = pb->pkg->imports.v[i];
    dlog("███████████████████████████████████████████████████████");
    if (( err = build_pkg(pkg, pb->c, /*outfile*/"", /*pkgbuildflags*/0) )) {
      dlog("error while building pkg %s: %s", pkg->path.p, err_str(err));
      break;
    }
    dlog("███████████████████████████████████████████████████████");
  }

  // trim excess space of imports array since we'll be keeping it around
  ptrarray_shrinkwrap(&pb->pkg->imports, pb->c->ma);

  return err;
}


err_t pkgbuild_typecheck(pkgbuild_t* pb) {
  compiler_t* c = pb->c;

  pkgbuild_begintask(pb, "typecheck");
  dlog_if(opt_trace_typecheck, "————————— typecheck —————————");

  // make sure there are no parse errors
  if (compiler_errcount(c) > 0) {
    dlog("%s called with pre-existing parse errors", __FUNCTION__);
    return ErrCanceled;
  }

  // typecheck
  err_t err = typecheck(c, pb->ast_ma, pb->pkg, pb->unitv, pb->unitc);
  if (err)
    return err;
  if (compiler_errcount(c) > 0) {
    dlog("typecheck failed with %u diagnostic errors", compiler_errcount(c));
    if (!opt_trace_parse && c->opt_printast)
      dump_pkg_ast(pb->pkg, pb->unitv, pb->unitc);
    return ErrCanceled;
  }

  // trace & dlog
  if (opt_trace_typecheck && c->opt_printast) {
    dlog("————————— AST after typecheck —————————");
    dump_pkg_ast(pb->pkg, pb->unitv, pb->unitc);
  }

  // dlog("abort");abort(); // XXX

  // analyze
  dlog_if(opt_trace_ir, "————————— IR —————————");
  err = analyze(c, pb->ast_ma, pb->pkg, pb->unitv, pb->unitc);
  if (err) {
    dlog("IR analyze: err=%s", err_str(err));
    return err;
  }
  if (compiler_errcount(c) > 0) {
    dlog("analyze failed with %u diagnostic errors", compiler_errcount(c));
    return ErrCanceled;
  }

  // print AST, if requested
  if (c->opt_printast) {
    if (opt_trace_typecheck || opt_trace_parse) {
      // we have printed the AST at various stages already,
      // so let's print a header to make it easier to distinguish what is what
      dlog("————————— AST after analyze —————————");
    }
    dump_pkg_ast(pb->pkg, pb->unitv, pb->unitc);
  }

  return 0;
}


err_t pkgbuild_metagen(pkgbuild_t* pb) {
  err_t err = 0;
  buf_t outbuf = buf_make(pb->c->ma);

  err = metagen(&outbuf, pb->c, pb->pkg, (const unit_t*const*)pb->unitv, pb->unitc);
  if (!err)
    err = fs_writefile("meta.lisp", 0644, buf_slice(outbuf));

  buf_dispose(&outbuf);
  return err;
}


static err_t cgen_api(pkgbuild_t* pb, cgen_t* g, cgen_pkgapi_t* pkgapi) {
  str_t pubhfile = pkg_buildfile(pb->pkg, pb->c, "pub.h");
  if UNLIKELY(pubhfile.len == 0)
    return ErrNoMem;

  if (pb->c->opt_verbose)
    pkgbuild_begintask(pb, "cgen %s", relpath(pubhfile.p));

  err_t err = cgen_pkgapi(g, (const unit_t**)pb->unitv, pb->unitc, pkgapi);
  if (err) {
    dlog("cgen_pkgapi: %s", err_str(err));
    goto end;
  }

  if (opt_trace_cgen) {
    fprintf(stderr, "—————————— cgen %s ——————————\n", relpath(pubhfile.p));
    fwrite(g->outbuf.p, g->outbuf.len, 1, stderr);
    fputs("\n——————————————————————————————————\n", stderr);
  }

  err = fs_writefile_mkdirs(pubhfile.p, 0660, pkgapi->pub_header);

end:
  str_free(pubhfile);
  return err;
}


err_t pkgbuild_cgen(pkgbuild_t* pb) {
  err_t err;
  compiler_t* c = pb->c;

  dlog_if(opt_trace_cgen, "————————— cgen —————————");
  assert(pb->pkg->files.len > 0);

  // create C code generator
  cgen_t g;
  if (!cgen_init(&g, c, pb->pkg, c->ma)) {
    dlog("cgen_init: %s", err_str(ErrNoMem));
    return ErrNoMem;
  }

  // create output dir and initialize cfiles & ofiles arrays
  err = prepare_builddir(pb);
  if (err)
    goto end;

  // generate package C header
  cgen_pkgapi_t pkgapi;
  err = cgen_api(pb, &g, &pkgapi);

  // generate one C file for each unit
  for (u32 i = 0; i < pb->unitc && !err; i++) {
    unit_t* unit = pb->unitv[i];
    const char* cfile = cfile_of_unit(pb, unit);

    if (pb->c->opt_verbose)
      pkgbuild_begintask(pb, "cgen %s", node_srcfilename((node_t*)unit, &c->locmap));

    if (( err = cgen_unit_impl(&g, unit, &pkgapi) ))
      break;

    if (opt_trace_cgen) {
      fprintf(stderr, "—————————— cgen %s ——————————\n", relpath(cfile));
      fwrite(g.outbuf.p, g.outbuf.len, 1, stderr);
      fputs("\n——————————————————————————————————\n", stderr);
    }

    err = fs_writefile_mkdirs(cfile, 0660, buf_slice(g.outbuf));
  }

  cgen_pkgapi_dispose(&g, &pkgapi);
end:
  cgen_dispose(&g);
  return err;
}


err_t pkgbuild_begin_late_compilation(pkgbuild_t* pb) {
  if (pb->pkg->files.len == 0)
    return 0;

  err_t err = 0;
  assertf(pb->ofiles.len > 0, "prepare_builddir not called");

  for (u32 i = 0; i < pb->pkg->files.len && err == 0; i++) {
    srcfile_t* srcfile = &pb->pkg->files.v[i];
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
  for (u32 i = 0; i < pb->pkg->files.len; i++) {
    err_t err1 = promise_await(&pb->promisev[i]);
    if (!err)
      err = err1;
  }
  return err;
}


static err_t link_exe(pkgbuild_t* pb, const char* outfile) {
  compiler_t* c = pb->c;
  err_t err = 0;

  // TODO: -Llibdir
  // char libflag[PATH_MAX];
  // snprintf(libflag, sizeof(libflag), "-L%s", c->libdir);

  // TODO: libfiles
  // assert(*c->builddir != 0);
  const char* libfiles[] = {
    path_join_alloca(pb->c->builddir, "pkg/std/runtime/libruntime.a"), // XXX FIXME
  };

  assert(pb->ofiles.len > 0);

  CoLLVMLink link = {
    .target_triple = c->target.triple,
    .outfile = outfile,
    .infilev = (const char*const*)strlist_array(&pb->ofiles),
    .infilec = pb->ofiles.len,
    .libfilev = libfiles,
    .libfilec = countof(libfiles),
    .sysroot = c->sysroot,
    .print_lld_args = coverbose > 1,
    .lto_level = 0,
    .lto_cachedir = "",
  };

  // configure LTO
  str_t lto_cachedir = {0};
  if (c->buildmode == BUILDMODE_OPT && !target_is_riscv(&c->target)) {
    lto_cachedir = pkg_builddir(pb->pkg, pb->c);
    bool ok = lto_cachedir.len > 0;
    ok &= str_push(&lto_cachedir, PATH_SEP);
    ok &= str_append(&lto_cachedir, "llvm");
    if (!ok) {
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

  str_t ofstr = {0};
  err_t err = 0;

  // build executable if there's a public main function, else build a library
  bool is_exe = true;
  void** mainfunp = map_lookup_ptr(&pb->pkg->defs, sym_main);
  if (!mainfunp || ((expr_t*)*mainfunp)->kind != EXPR_FUN)
    is_exe = false;

  // if no outfile is given, use the default one:
  // exe: "{builddir}/bin/{short_pkg_name}"
  // lib: "{pkgbuilddir}/lib{short_pkg_name}.a"
  if (*outfile == 0) {
    bool ok = true;
    if (is_exe) {
      ofstr = str_make(pb->c->builddir);
      ok &= str_append(&ofstr, PATH_SEP_STR "bin" PATH_SEP_STR);
      ok &= str_append(&ofstr, path_base(pb->pkg->path.p));
    } else {
      ofstr = pkg_builddir(pb->pkg, pb->c);
      ok &= str_append(&ofstr, PATH_SEP_STR "lib");
      ok &= str_append(&ofstr, path_base(pb->pkg->path.p));
      ok &= str_append(&ofstr, ".a");
    }
    if (!ok) {
      str_free(ofstr);
      return ErrNoMem;
    }
    outfile = ofstr.p;
  }

  pkgbuild_begintask(pb, "link %s", relpath(outfile));

  char* dir = path_dir_alloca(outfile);
  if (( err = fs_mkdirs(dir, 0755, FS_VERBOSE) ))
    return err;

  if (is_exe) {
    err = link_exe(pb, outfile);
  } else {
    err = link_lib_archive(pb, outfile);
  }

  bgtask_end(pb->bgt, "%s",
    (pb->flags & PKGBUILD_NOLINK) ? "(compile only)" :
    relpath(outfile));

  str_free(ofstr);
  return err;
}


err_t build_pkg(
  pkg_t* pkg, compiler_t* c, const char* outfile, u32 pkgbuild_flags)
{
  err_t err;

  if (compiler_errcount(c) > 0) {
    // TODO: consider returning ErrCanceled in this case
    dlog("-- WARNING -- build_pkg with a compiler that has encountered hard errors");
  }

  pkgbuild_t pb;
  if (( err = pkgbuild_init(&pb, pkg, c, pkgbuild_flags) ))
    return err;

  #define DO_STEP(fn, args...) \
    if (( err = fn(&pb, ##args) )) { \
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

  // generate package metadata (can run in parallel to the rest of these tasks)
  #ifdef ENABLE_METAGEN
  DO_STEP(pkgbuild_metagen);
  #endif

  // generate C code for package
  DO_STEP(pkgbuild_cgen);

  // begin compilation of C source files generated from compis sources
  DO_STEP(pkgbuild_begin_late_compilation);

  // wait for compilation tasks to finish
  DO_STEP(pkgbuild_await_compilation);

  // link exe or library (does nothing if PKGBUILD_NOLINK flag is set)
  DO_STEP(pkgbuild_link, outfile);

end:
  pkgbuild_dispose(&pb); // TODO: can skip this for top-level package
  #undef DO_STEP
  return err;
}