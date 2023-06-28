// SPDX-License-Identifier: Apache-2.0
#include "colib.h"
#include "path.h"
#include "compiler.h"
#include "subproc.h"
#include "bgtask.h"
#include "dirwalk.h"
#include "sha256.h"
#include "thread.h"
#include "hash.h"
#include "chan.h"

#include <stdlib.h> // exit
#include <unistd.h> // getopt
#include <string.h> // strdup
#include <err.h>
#include <errno.h>
#include <getopt.h>

#include "llvm/llvm.h"

// cli options
static bool opt_help = false;
static const char* opt_out = "";
static const char* opt_targetstr = "";
static const target_t* opt_target = NULL;
static bool opt_debug = false;
static bool opt_verbose = false;
static const char* opt_maxproc = "";
static bool opt_printast = false;
static bool opt_printir = false;
static bool opt_genirdot = false;
static bool opt_genasm = false;
static bool opt_logld = false;
static bool opt_nolink = false;
static bool opt_nomain = false;
static bool opt_version = false;
static const char* opt_builddir = "build";
#if DEBUG
  static bool opt_trace_all = false;
  bool opt_trace_parse = false;
  bool opt_trace_typecheck = false;
  bool opt_trace_comptime = false;
  bool opt_trace_ir = false;
  bool opt_trace_cgen = false;
  bool opt_trace_subproc = false;
#endif

#define FOREACH_CLI_OPTION(S, SV, L, LV,  DEBUG_L, DEBUG_LV) \
  /* S( var, ch, name,          descr) */\
  /* SV(var, ch, name, valname, descr) */\
  /* L( var,     name,          descr) */\
  /* LV(var,     name, valname, descr) */\
  SV(&opt_out,    'o', "out","<file>", "Write product to <file> instead of build dir")\
  S( &opt_debug,  'd', "debug",     "Build in debug aka development mode")\
  S( &opt_verbose,'v', "verbose",   "Verbose mode prints extra information")\
  SV(&opt_maxproc,'j', "maxproc","<N>", "Use up to N parallel processes/threads")\
  S( &opt_genasm, 'S', "write-asm", "Write machine assembly sources to build dir")\
  S( &opt_help,   'h', "help",      "Print help on stdout and exit")\
  /* advanced options (long form only) */ \
  LV(&opt_targetstr,"target", "<target>", "Build for <target> instead of host")\
  LV(&opt_builddir, "build-dir", "<dir>", "Use <dir> instead of ./build")\
  L( &opt_printast, "print-ast",    "Print AST to stderr")\
  L( &opt_printir,  "print-ir",     "Print IR to stderr")\
  L( &opt_genirdot, "write-ir-dot", "Write IR as Graphviz .dot file to build dir")\
  L( &opt_logld,    "print-ld-cmd", "Print linker invocation to stderr")\
  L( &opt_nolink,   "no-link",      "Only compile, don't link")\
  L( &opt_nomain,   "no-auto-main", "Don't auto-generate C ABI \"main\" for main.main")\
  L( &opt_version,  "version",      "Print Compis version on stdout and exit")\
  /* debug-only options */\
  DEBUG_L( &opt_trace_all,       "trace",           "Trace everything")\
  DEBUG_L( &opt_trace_parse,     "trace-parse",     "Trace parsing")\
  DEBUG_L( &opt_trace_typecheck, "trace-typecheck", "Trace type checking")\
  DEBUG_L( &opt_trace_comptime,  "trace-comptime",  "Trace comptime eval")\
  DEBUG_L( &opt_trace_ir,        "trace-ir",        "Trace IR")\
  DEBUG_L( &opt_trace_cgen,      "trace-cgen",      "Trace code generation")\
  DEBUG_L( &opt_trace_subproc,   "trace-subproc",   "Trace subprocess execution")\
// end FOREACH_CLI_OPTION

#include "cliopt.inc.h"

static void help(const char* prog) {
  printf(
    "Compis " CO_VERSION_STR ", your friendly neighborhood compiler\n"
    "Usage: %s %s [options] [--] <package>\n"
    "       %s %s [options] [--] <sourcedir>\n"
    "       %s %s [options] [--] <sourcefile> ...\n"
    "Options:\n"
    "",
    coprogname, prog,
    coprogname, prog,
    coprogname, prog);
  print_options();
  exit(0);
}

typedef u8 buildflags_t;
#define BUILD_NOLINK ((u8)1 << 0)  // don't link


static err_t build_pkg(
  pkg_t* pkg, const compiler_config_t* ccfg, const char* outfile, buildflags_t);
static err_t build_dep_pkg(str_t pkgname, const compiler_config_t* ccfg);


static void set_comaxproc() {
  char* end;
  unsigned long n = strtoul(opt_maxproc, &end, 10);
  if (n == ULONG_MAX || n > U32_MAX || *end || (n == 0 && errno))
    errx(1, "invalid value for -j: %s", opt_maxproc);
  if (n != 0) {
    comaxproc = (u32)n;
    dlog("setting comaxproc=%u from -j option", comaxproc);
  }
}


int main_build(int argc, char* argv[]) {
  int optind = parse_cli_options(argc, argv, help);
  if (optind < 0)
    return 1;

  coverbose |= opt_verbose;
  #if DEBUG
    // --co-trace turns on all trace flags
    opt_trace_parse |= opt_trace_all;
    opt_trace_typecheck |= opt_trace_all;
    opt_trace_comptime |= opt_trace_all;
    opt_trace_ir |= opt_trace_all;
    opt_trace_cgen |= opt_trace_all;
    opt_trace_subproc |= opt_trace_all;
  #endif

  if (opt_version) {
    print_co_version();
    return 0;
  }

  if (optind == argc)
    errx(1, "no input");

  if (*opt_maxproc)
    set_comaxproc();

  assert(optind <= argc);
  argv += optind;
  argc -= optind;

  if (opt_nolink && *opt_out) {
    elog("cannot specify both --no-link and -o (nothing to output when not linking)");
    return 1;
  }

  // configure target
  if (!( opt_target = target_find(opt_targetstr) )) {
    elog("Invalid target \"%s\"", opt_targetstr);
    elog("See `%s targets` for a list of supported targets", relpath(coexefile));
    return 1;
  }
  #if DEBUG
    char tmpbuf[TARGET_FMT_BUFCAP];
    target_fmt(opt_target, tmpbuf, sizeof(tmpbuf));
    dlog("targeting %s (%s)", tmpbuf, opt_target->triple);
  #endif

  // decide what package to build
  pkg_t* pkgv;
  u32 pkgc;
  err_t err = 0;
  if (( err = pkgs_for_argv(argc, argv, &pkgv, &pkgc) ))
    return 1;
  #if DEBUG
    printf("[D] building %u package%s:", pkgc, pkgc != 1 ? "s" : "");
    for (u32 i = 0; i < pkgc; i++)
      printf(&", %s (\"%s\")"[((u32)!i)], pkgv[i].name.p, pkgv[i].dir.p);
    printf("\n");
  #endif

  // // for now we limit ourselves to building one package per compis invocation
  // if (pkgc > 1) {
  //   errx(1, "more than one package requested; please only specify one package");
  // }
  if (pkgc > 1 && *opt_out)
    errx(1, "cannot specify -o option when building multiple packages");

  // configuration for building requested packages
  compiler_config_t ccfg = {
    .target = opt_target,
    .buildroot = opt_builddir,
    .buildmode = opt_debug ? BUILDMODE_DEBUG : BUILDMODE_OPT,
    .printast = opt_printast,
    .printir = opt_printir,
    .genirdot = opt_genirdot,
    .genasm = opt_genasm,
    .verbose = coverbose,
    .nomain = opt_nomain,
  };

  // // XXX FIXME
  // if (!streq(pkgv[0].name.p, "std/runtime")) {
  //   if (( err = build_dep_pkg(str_make("std/runtime"), &ccfg) ))
  //     return 1;
  // }

  // build packages
  buildflags_t buildflags = 0;
  if (opt_nolink)
    buildflags |= BUILD_NOLINK;
  for (u32 i = 0; i < pkgc; i++) {
    if (( err = build_pkg(&pkgv[i], &ccfg, opt_out, buildflags) )) {
      dlog("error while building pkg %s: %s", pkgv[i].name.p, err_str(err));
      break;
    }
  }

  return (int)!!err;
}


static err_t build_dep_pkg(str_t pkgname, const compiler_config_t* parent_ccfg) {
  pkg_t pkg = {
    .name = pkgname,
  };
  if (!pkg_find_dir(&pkg)) {
    elog("package not found: %s", pkgname.p);
    return ErrNotFound;
  }

  compiler_config_t ccfg = *parent_ccfg;
  ccfg.printast = false;
  ccfg.printir = false;
  ccfg.genirdot = false;
  ccfg.genasm = false;
  ccfg.nomain = false;

  err_t err = build_pkg(&pkg, &ccfg, "", /*flags*/0);
  if (err)
    dlog("error while building pkg %s: %s", pkg.name.p, err_str(err));

  return err;
}


static void diaghandler(const diag_t* d, void* nullable userdata) {
  elog("%s", d->msg);
  if (d->srclines && *d->srclines)
    elog("%s", d->srclines);
}


static err_t link_exe(
  compiler_t* c, bgtask_t* task, const char* outfile, char*const* infilev, u32 infilec)
{
  // TODO: -Llibdir
  // char libflag[PATH_MAX];
  // snprintf(libflag, sizeof(libflag), "-L%s", c->libdir);

  const char* libfiles[] = {
    path_join_alloca(c->builddir, "std/runtime.a"),
  };

  CoLLVMLink link = {
    .target_triple = c->target.triple,
    .outfile = outfile,
    .infilev = (const char*const*)infilev,
    .infilec = infilec,
    .libfilev = libfiles,
    .libfilec = countof(libfiles),
    .sysroot = c->sysroot,
    .print_lld_args = coverbose || opt_logld,
    .lto_level = 0,
    .lto_cachedir = "",
  };

  if (c->buildmode == BUILDMODE_OPT && !target_is_riscv(&c->target)) {
    link.lto_level = 2;
    link.lto_cachedir = path_join_alloca(c->builddir, "llvm");
  }

  err_t err = 0;

  task->n++;
  bgtask_setstatusf(task, "link %s", relpath(outfile));
  err = llvm_link(&link);

  return err;
}


static err_t archive_lib(
  compiler_t* c, bgtask_t* task, const char* outfile, char*const* objv, u32 objc)
{
  err_t err = 0;
  assert(objc <= (usize)U32_MAX);

  task->n++;
  bgtask_setstatusf(task, "create %s", relpath(outfile));

  char* dir = path_dir_alloca(outfile);
  if (( err = fs_mkdirs(dir, 0755, FS_VERBOSE) ))
    return err;

  CoLLVMArchiveKind arkind;
  if (c->target.sys == SYS_none) {
    arkind = llvm_sys_archive_kind(target_default()->sys);
  } else {
    arkind = llvm_sys_archive_kind(c->target.sys);
  }

  char* errmsg = "?";
  err = llvm_write_archive(arkind, outfile, (const char*const*)objv, objc, &errmsg);

  if (err) {
    elog("llvm_write_archive: (err=%s) %s", err_str(err), errmsg);
    if (err == ErrNotFound) {
      for (u32 i = 0; i < objc; i++) {
        if (!fs_isfile(objv[i]))
          elog("%s: file not found", objv[i]);
      }
    }
    LLVMDisposeMessage(errmsg);
  }

  return err;
}


static bool pkg_is_built(pkg_t* pkg, compiler_t* c) {
  // str_t statusfile = path_join(c->pkgbuilddir, "status.toml");
  // unixtime_t status_mtime = fs_mtime(statusfile.p);
  // unixtime_t source_mtime = pkg_source_mtime(pkg);

  // TODO: Read "default_outfile" from statusfile and check its mtime and existence
  //       Use const char* opt_out if set.

  return false;
}


static char*const* pkg_mkobjfilelist(pkg_t* pkg, compiler_t* c, strlist_t* objfiles) {
  str_t s = {0};
  strlist_init(objfiles, c->ma);

  for (u32 i = 0; i < pkg->files.len; i++) {
    srcfile_t* srcfile = &pkg->files.v[i];

    // {pkgbuilddir}/{srcfile}.o
    // note that pkgbuilddir includes pkgname
    s.len = 0;
    str_append(&s, c->pkgbuilddir);
    str_push(&s, PATH_SEPARATOR);
    str_appendlen(&s, srcfile->name.p, srcfile->name.len);
    if UNLIKELY(!str_append(&s, ".o"))
      goto oom;

    strlist_add_raw(objfiles, s.p, s.len, 1);
  }

  assert(objfiles->len == pkg->files.len);
  char*const* objfilev = strlist_array(objfiles);
  if (!objfiles->ok)
    goto oom;

  str_free(s);

  return objfilev;
oom:
  panic("out of memory");
}


static err_t compile_c_source(
  compiler_t* c, promise_t* promise, srcfile_t* srcfile, str_t cfile, const char* ofile)
{
  // Use package as working directory for subprocesses.
  // ofile must not be relative because of this.
  assert(path_isabs(ofile));
  const char* wdir = srcfile->pkg->dir.p;

  // subprocs attached to promise
  subprocs_t* subprocs = subprocs_create_promise(c->ma, promise);
  if (!subprocs)
    return ErrNoMem;

  // compile C -> object
  err_t err = compile_c_to_obj_async(c, subprocs, wdir, cfile.p, ofile);

  // compile C -> asm
  if (!err && c->opt_genasm)
    err = compile_c_to_asm_async(c, subprocs, wdir, cfile.p, ofile);

  if UNLIKELY(err)
    subprocs_cancel(subprocs);
  return err;
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


static err_t co_parse_unit(
  compiler_t* c,
  memalloc_t ast_ma,
  parser_t* parser,
  srcfile_t* srcfile,
  unit_t** result)
{
  err_t err = 0;

  if (( err = srcfile_open(srcfile) )) {
    elog("%s: %s", srcfile->name.p, err_str(err));
    return err;
  }

  dlog_if(opt_trace_parse, "————————— parse —————————");
  *result = parser_parse(parser, ast_ma, srcfile);
  if (c->errcount > 0) // FIXME don't read thread-shared c->errcount
    return ErrCanceled;

  dlog("parsed %s", srcfile->name.p);
  return 0;
}


static err_t co_transfer_pkgdefs(compiler_t* c, map_t* pkgdefs, map_t* tfuns) {
  pkg_t* pkg = c->pkg;

  if UNLIKELY(
    !map_reserve(&pkg->defs, c->ma, pkgdefs->len) ||
    !map_reserve(&pkg->tfuns, c->ma, tfuns->len))
  {
    return ErrNoMem;
  }

  for (const mapent_t* e = map_it(pkgdefs); map_itnext(pkgdefs, &e); ) {
    dlog("pkgdef %s", (sym_t)e->key);
    void** vp = assertnotnull(map_assign_ptr(&pkg->defs, c->ma, e->key));
    if (*vp) {
      elog("TODO handle duplicate package-level definition (%s)", (sym_t)e->key);
    } else {
      *vp = e->value;
    }
  }
  map_clear(pkgdefs);

  for (const mapent_t* e = map_it(tfuns); map_itnext(tfuns, &e); ) {
    dlog("tfun %s", (sym_t)e->key);
    void** vp = assertnotnull(map_assign_ptr(&pkg->tfuns, c->ma, e->key));
    if (*vp) {
      elog("TODO handle duplicate package-level type function (%s)", (sym_t)e->key);
    } else {
      *vp = e->value;
    }
  }
  map_clear(tfuns);

  return 0;
}


static err_t co_analyze_unit(compiler_t* c, unit_t* unit, memalloc_t ast_ma) {
  err_t err = 0;
  // bool printed_trace_ast = false;

  dlog_if(opt_trace_typecheck, "————————— typecheck —————————");
  if (( err = typecheck(c, unit, ast_ma) ))
    return err;
  if (c->errcount > 0) // FIXME better way to detect errors
    return ErrCanceled;

  if (opt_trace_typecheck && c->opt_printast) {
    dlog("————————— AST after typecheck —————————");
    // printed_trace_ast = true;
    dump_ast((node_t*)unit);
  }

  // dlog("abort");abort(); // XXX

  dlog_if(opt_trace_ir, "————————— IR —————————");
  if (( err = analyze(c, unit, ast_ma) )) {
    dlog("IR analyze: err=%s", err_str(err));
    return err;
  }
  if (c->errcount > 0) // FIXME better way to detect errors
    return ErrCanceled;

  return 0;
}


static err_t co_cgen_unit(compiler_t* c, unit_t* unit, const char* cfile) {
  dlog_if(opt_trace_cgen, "————————— cgen —————————");

  cgen_t g;
  if (!cgen_init(&g, c, c->ma)) {
    dlog("cgen_init");
    return ErrNoMem;
  }

  err_t err = cgen_generate(&g, unit);
  if (!err) {
    if (opt_trace_cgen) {
      fprintf(stderr, "——————————\n%.*s\n——————————\n",
        (int)g.outbuf.len, g.outbuf.chars);
    }
    srcfile_t* srcfile = loc_srcfile(unit->loc, &c->locmap);
    dlog("cgen %s -> %s", srcfile ? srcfile->name.p : "<input>", cfile);
    err = fs_writefile(cfile, 0660, buf_slice(g.outbuf));
  }

  cgen_dispose(&g);
  return err;
}


typedef struct {
  compiler_t* c;
  sema_t*     step2_sema; // supervisor signals to all cworkers that all parsing is done
  sema_t*     end_sema;   // cworkers signals to supervisor when it's done
  chan_t*     jobch;
  chan_t*     parseresultch;
  chan_t*     resultch;
  thrd_t      t;
} cworker_t;

typedef struct {
  u32        id;
  srcfile_t* srcfile; // borrowed
  str_t      cfile;   // borrowed
} cjobinfo_t;

typedef struct {
  map_t pkgdefs;
  map_t tfuns;
} cparseresult_t;

typedef struct {
  u32   id;
  err_t err;
} cjobresult_t;

typedef struct {
  u32              id;
  srcfile_t*       srcfile; // borrowed
  str_t            cfile;   // borrowed
  unit_t* nullable unit;
  err_t            err;
} cjob_t;


static void job_dispose(cjob_t* job) {
  srcfile_close(job->srcfile);
}


static err_t cworker_thread(cworker_t* cw) {
  // dlog("[cworker %p] BEGIN", cw);
  err_t err = 0;

  memalloc_t ma = cw->c->ma;

  // allocate a slab of memory for AST and IR
  usize ast_memsize = 1024*1024*8lu; // 8 MiB for AST & IR
  memalloc_t ast_ma = memalloc_bump_in_zeroed(ma, ast_memsize, /*flags*/0);
  if (ast_ma == memalloc_null()) {
    elog("failed to allocate %zu MiB memory for AST", ast_memsize/(1024*1024lu));
    return ErrNoMem;
  }

  // create parser
  parser_t parser;
  if (!parser_init(&parser, cw->c)) {
    memalloc_bump_in_dispose(ast_ma);
    return ErrNoMem;
  }

  // dlog("[cworker %p] waiting for work on jobch", cw);
  array_type(cjob_t) jobs = {0};
  cjobinfo_t jobinfo;
  while (chan_recv(cw->jobch, &jobinfo)) {
    dlog("cworker %p accepted job#%u: %s", cw, jobinfo.id, jobinfo.srcfile->name.p);

    usize ast_mem_avail = memalloc_bumpcap(ast_ma) - memalloc_bumpuse(ast_ma);
    dlog("remaining AST memory: %zu", ast_mem_avail);
    // TODO: allocate another ast_ma if the current one is low on free space
    if (ast_mem_avail < 1024*1024)
      panic("TODO: implement incremental AST memory");

    cjob_t* job = safechecknotnull(array_alloc(cjob_t, (array_t*)&jobs, ma, 1));
    job->id = jobinfo.id;
    job->srcfile = jobinfo.srcfile;
    job->cfile = jobinfo.cfile;
    job->err = co_parse_unit(cw->c, ast_ma, &parser, job->srcfile, &job->unit);
    if (job->err)
      elog("job %s failed: %s", job->srcfile->name.p, err_str(job->err));
  }

  // Channel closed; all jobs have been parsed

  // Simulate being slow
  //microsleep(fastrand() % 1000000);

  // send parse results to supervisor
  cparseresult_t parseresult = {0};
  parseresult.pkgdefs = parser.pkgdefs; parser.pkgdefs = (map_t){0};
  parseresult.tfuns = parser.tfuns; parser.tfuns = (map_t){0};
  safecheckf(chan_send(cw->parseresultch, &parseresult), "chan_send");

  // Wait for a signal per job from the supervisor, which tells us that
  // all package symbols are available, which we need for the analysis stage.
  sema_wait(cw->step2_sema);

  // continue & finalize all jobs
  for (u32 i = 0; i < jobs.len; i++) {
    cjob_t* job = &jobs.v[i];
    if (!job->err)
      job->err = co_analyze_unit(cw->c, job->unit, ast_ma);
    if (!job->err)
      job->err = co_cgen_unit(cw->c, job->unit, job->cfile.p);
    cjobresult_t result = { job->id, job->err };
    safecheckf(chan_send(cw->resultch, &result), "chan_send");
  }

  // wait for supervisor to be done with AST access
  sema_wait(cw->end_sema);

  // dispose of resources
  for (u32 i = 0; i < jobs.len; i++)
    job_dispose(&jobs.v[i]);
  array_dispose(cjob_t, (array_t*)&jobs, ma);
  parser_dispose(&parser);
  memalloc_bump_in_dispose(ast_ma);

  //dlog("[cworker %p] exit", cw);
  return err;
}


static err_t cworker_spawn(cworker_t* cw) {
  errno = 0;
  int err = thrd_create(&cw->t, (thrd_start_t)cworker_thread, cw);
  return err == 0 ? 0 :
         errno ? err_errno() :
         ErrCanceled;
}


// static err_t cworker_wait(cworker_t* cw) {
//   err_t err = ErrInvalid;
//   int r = thrd_join(cw->t, &err);
//   if (r != 0) {
//     err = errno ? err_errno() : ErrCanceled;
//     dlog("thrd_join failed: %s", err_str(err));
//   }
//   return err;
// }


static str_t cfile_make_from_ofile(const char* ofile) {
  assert(strlen(ofile) > 0 && ofile[strlen(ofile)-1] == 'o');
  str_t cfile = str_make(ofile);
  safecheck(cfile.len > 0);
  cfile.p[cfile.len - 1] = 'c'; // x.o -> x.c
  return cfile;
}


static err_t compile_pkg_co_sources(
  pkg_t* pkg, compiler_t* c, bgtask_t* bgtask, const char*const* ofilev,
  promise_t* promisev, u32 ncosrc)
{
  sema_t step2_sema, end_sema;
  safecheck(sema_init(&step2_sema, 0) == 0);
  safecheck(sema_init(&end_sema, 0) == 0);

  chan_t* jobch = chan_open(c->ma, sizeof(cjobinfo_t), /*buffer size*/ncosrc);
  safecheckf(jobch, "chan_open");

  chan_t* resultch = chan_open(c->ma, sizeof(cjobresult_t), /*buffer size*/ncosrc);
  safecheckf(resultch, "chan_open");

  chan_t* parseresultch = chan_open(
    c->ma, sizeof(cparseresult_t), /*buffer size*/ncosrc);
  safecheckf(parseresultch, "chan_open");

  str_t* cfilev = mem_alloctv(c->ma, str_t, (usize)ncosrc);
  safecheckf(cfilev, "out of memory");

  // allocate workers
  // spawn at most comaxproc workers, but no more than there are source files
  u32 cwc = MIN(comaxproc, ncosrc);
  cworker_t* cwv = mem_alloctv(c->ma, cworker_t, (usize)cwc);
  safecheckf(cwv, "out of memory");

  // spawn workers
  err_t err = 0;
  for (u32 i = 0; i < cwc; i++) {
    cworker_t* cw = &cwv[i];
    cw->c = c;
    cw->step2_sema = &step2_sema;
    cw->end_sema = &end_sema;
    cw->jobch = jobch;
    cw->parseresultch = parseresultch;
    cw->resultch = resultch;
    err = cworker_spawn(cw);
    safecheckf(err == 0, "cworker_spawn");
  }

  // set filename for intermediate C file
  for (u32 i = 0; i < ncosrc; i++)
    cfilev[i] = cfile_make_from_ofile(ofilev[i]);

  // begin compilation of each source file
  for (u32 i = 0; i < pkg->files.len && err == 0; i++) {
    srcfile_t* srcfile = &pkg->files.v[i];
    if (srcfile->type != FILE_CO)
      continue;
    bgtask->n++;
    bgtask_setstatusf(bgtask, "compile %s", relpath(srcfile->name.p));
    cjobinfo_t job = { i, srcfile, cfilev[i] };
    safecheckf(chan_send(jobch, &job), "chan_send");
  }

  // signal to workers that all jobs have been submitted
  chan_close(jobch);

  // wait for all cworkers to finish parsing.
  // As we receive package-level definitions from each worker, transfer them to pkg
  cparseresult_t parseresult;
  for (u32 i = 0; i < cwc; i++) {
    safecheckf(chan_recv(parseresultch, &parseresult), "channel closed prematurely");
    dlog("got parse results from cw %p", &cwv[i]);
    if (!err)
      err = co_transfer_pkgdefs(c, &parseresult.pkgdefs, &parseresult.tfuns);
    map_dispose(&parseresult.pkgdefs, c->ma);
    tfunmap_dispose(&parseresult.tfuns, c->ma);
  }
  chan_close(parseresultch);

  // signal to all workers to continue, now that all parsing has completed
  sema_signal(&step2_sema, cwc);

  // receive results and dispatch .c -> .o compilation
  cjobresult_t result;
  for (u32 i = 0; i < ncosrc; i++) {
    safecheckf(chan_recv(resultch, &result), "channel closed prematurely");
    dlog("received result of job#%u: %s", result.id, err_str(result.err));
    if (result.err && !err)
      err = result.err;
    if (!err) {
      promise_t* p = &promisev[result.id];
      srcfile_t* srcfile = &pkg->files.v[result.id];
      err = compile_c_source(c, p, srcfile, cfilev[result.id], ofilev[result.id]);
    }
  }
  chan_close(resultch);

  // signal to cworkers that we are done and that they can exit.
  // we must do this to avoid race on AST access from ast_ma slabs.
  sema_signal(&end_sema, cwc);

  // Note: we don't need to wait for the workers threads to exit.
  // Just let them exit by themselves and keep this thread here moving forward.
  // // wait for all cworkers to end
  // for (u32 i = 0; i < cwc; i++) {
  //   cworker_t* cw = &cwv[i];
  //   dlog("cworker_wait(%p)", cw);
  //   err_t err1 = cworker_wait(cw);
  //   if (err1) {
  //     dlog("cworker_wait %p returned error: %s", cw, err_str(err1));
  //     err = err ? err : err1;
  //   }
  // }

  mem_freetv(c->ma, cfilev, (usize)ncosrc);
  mem_freetv(c->ma, cwv, (usize)cwc);
  sema_dispose(&step2_sema);
  sema_dispose(&end_sema);
  return 0;
}


// compile_pkg_co_source builds a single compis source file,
// for packages that have only one compis source; avoids the whole cworker setup.
static err_t compile_pkg_co_source(
  pkg_t* pkg, compiler_t* c, bgtask_t* bgtask,
  srcfile_t* srcfile, const char* ofile, promise_t* promise)
{
  err_t err = 0;

  bgtask->n++;
  bgtask_setstatusf(bgtask, "compile %s", relpath(srcfile->name.p));

  // allocate a slab of memory for AST and IR
  usize ast_memsize = 1024*1024*8lu; // 8 MiB for AST & IR
  memalloc_t ast_ma = memalloc_bump_in_zeroed(c->ma, ast_memsize, /*flags*/0);
  if (ast_ma == memalloc_null()) {
    elog("failed to allocate %zu MiB memory for AST", ast_memsize/(1024*1024lu));
    return ErrNoMem;
  }

  // create parser
  parser_t parser;
  if (!parser_init(&parser, c)) {
    memalloc_bump_in_dispose(ast_ma);
    return ErrNoMem;
  }

  // parse
  unit_t* unit = NULL;
  err = co_parse_unit(c, ast_ma, &parser, srcfile, &unit);

  // transfer package-level definitions to package
  assert(pkg->defs.len == 0);
  assert(pkg->tfuns.len == 0);
  map_dispose(&pkg->defs, c->ma);
  map_dispose(&pkg->tfuns, c->ma);
  pkg->defs = parser.pkgdefs; parser.pkgdefs = (map_t){0};
  pkg->tfuns = parser.tfuns; parser.tfuns = (map_t){0};

  // analyze
  if (!err)
    err = co_analyze_unit(c, unit, ast_ma);

  // generate C code
  str_t cfile = cfile_make_from_ofile(ofile);
  if (!err)
    err = co_cgen_unit(c, unit, cfile.p);

  // compile .c -> .o
  if (!err)
    err = compile_c_source(c, promise, srcfile, cfile, ofile);

  parser_dispose(&parser);
  memalloc_bump_in_dispose(ast_ma);

  return err;
}


static err_t compile_pkg_other_sources(
  pkg_t* pkg, compiler_t* c, bgtask_t* bgtask, const char*const* ofilev,
  promise_t* promisev)
{
  err_t err = 0;
  for (u32 i = 0; i < pkg->files.len && err == 0; i++) {
    srcfile_t* srcfile = &pkg->files.v[i];
    switch (srcfile->type) {
    case FILE_C:
      bgtask->n++;
      bgtask_setstatusf(bgtask, "compile %s", relpath(srcfile->name.p));
      err = compile_c_source(c, &promisev[i], srcfile, srcfile->name, ofilev[i]);
      break;
    case FILE_CO:
      break;
    default:
      log("%s: unrecognized file type", srcfile->name.p);
      err = ErrNotSupported;
    }
  }
  return err;
}


static err_t compile_pkg(
  pkg_t* pkg, compiler_t* c, bgtask_t* bgtask, const char*const* ofilev)
{
  err_t err = 0;

  // thread 1                    thread 2                    thread 3
  // ——————————————————————————  ——————————————————————————  ——————————————————————————
  // parse   a.co → a.ast        parse   b.co → b.ast        compile c.c → c.o
  // await parse fence           await parse fence           END
  // analyze a.ast → a.ir        analyze b.ast → b.ir
  // cgen    a.ast → a.c         cgen    b.ast → b.c
  // compile a.c → a.o           compile b.c → b.o
  // END                         END
  // link a.o b.o c.o → pkg
  //

  // allocate array of promises
  promise_t* promisev = mem_alloctv(c->ma, promise_t, (usize)pkg->files.len);
  safecheckf(promisev, "out of memory");

  // count number of compis source files
  u32 ncosrc = 0;
  for (u32 i = 0; i < pkg->files.len; i++)
    ncosrc += (u32)(pkg->files.v[i].type == FILE_CO);

  // different implementations of source unit-compilation dispatch for co vs c
  if (ncosrc != pkg->files.len)
    compile_pkg_other_sources(pkg, c, bgtask, ofilev, promisev);
  if (ncosrc > 0) {
    // note: this function blocks until all co sources have been compiled to c,
    // so this should be called after compile_pkg_other_sources
    if (ncosrc == 1) {
      for (u32 i = 0; i < pkg->files.len; i++) {
        if (pkg->files.v[i].type != FILE_CO)
          continue;
        compile_pkg_co_source(pkg, c, bgtask, &pkg->files.v[i], ofilev[i], &promisev[i]);
        break;
      }
    } else {
      compile_pkg_co_sources(pkg, c, bgtask, ofilev, promisev, ncosrc);
    }
  }

  // wait for all compiler promises
  for (u32 i = 0; i < pkg->files.len; i++) {
    err_t err1 = promise_await(&promisev[i]);
    if (!err)
      err = err1;
  }

  mem_freetv(c->ma, promisev, (usize)pkg->files.len);
  return err;
}


static err_t build_pkg(
  pkg_t* pkg, const compiler_config_t* ccfg, const char* outfile, buildflags_t flags)
{
  err_t err = 0;

  // make sure we have source files
  if (pkg->files.len == 0)
    pkg_find_files(pkg);
  if (pkg->files.len == 0) {
    elog("[%s] no source files in %s", pkg->name.p, relpath(pkg->dir.p));
    return ErrNotFound;
  }

  compiler_t c;
  compiler_init(&c, memalloc_ctx(), &diaghandler, pkg);
  if (err || ( err = compiler_configure(&c, ccfg) )) {
    dlog("error in compiler_configure: %s", err_str(err));
    goto end1;
  }

  // check if package is already built
  if (pkg_is_built(pkg, &c)) {
    log("[%s] up to date", pkg->name.p); // in lieu of bgtask
    goto end1;
  }

  // build sysroot if needed
  if (( err = build_sysroot_if_needed(&c, /*flags*/0) ))
    goto end1;

  // create output dir
  if (( err = fs_mkdirs(c.pkgbuilddir, 0770, FS_VERBOSE) ))
    goto end1;

  // create list of object files
  strlist_t objfiles;
  char*const* ofilev = pkg_mkobjfilelist(pkg, &c, &objfiles);

  // create objfile directories, if needed.
  // note: only "main" package may have srcfiles with subdirectories.
  if (streq(pkg->name.p, "main")) {
    err = fs_mkdirs_for_files(c.ma, (const char*const*)ofilev, (usize)pkg->files.len);
    if (err)
      goto end2;
  }

  // configure a bgtask
  int taskflags = c.opt_verbose ? BGTASK_NOFANCY : 0;
  u32 tasklen = pkg->files.len + (u32)!(flags & BUILD_NOLINK);
  bgtask_t* task = bgtask_start(c.ma, pkg->name.p, tasklen, taskflags);

  // initialize package definition map, if needed
  if (pkg->defs.cap == 0)
    map_init(&pkg->defs, c.ma, /*lenhint*/32);

  // compile all sources
  err = compile_pkg(pkg, &c, task, (const char*const*)ofilev);

  // create executable if there's a main function
  if (!err && !(flags & BUILD_NOLINK)) {
    if (c.mainfun) {
      if (!*outfile)
        outfile = path_join_alloca(c.builddir, c.pkg->name.p);
      err = link_exe(&c, task, outfile, ofilev, pkg->files.len);
    } else {
      // create static library
      if (!*outfile)
        outfile = path_join_alloca(c.builddir, strcat_alloca(c.pkg->name.p, ".a"));
      err = archive_lib(&c, task, outfile, ofilev, pkg->files.len);
    }
  }

  // cleanup object files
  if (!err) for (u32 i = 0; i < pkg->files.len; i++) {
    if (unlink(ofilev[i]) != 0)
      elog("warning: failed to remove %s: %s", ofilev[i], err_str(err_errno()));
  }

  // end bgtask
  bgtask_end(task, "%s", (flags & BUILD_NOLINK) ? "(compile only)" : relpath(outfile));

end2:
  strlist_dispose(&objfiles);

  for (u32 i = 0; i < pkg->files.len; i++)
    srcfile_close(&pkg->files.v[i]);
end1:
  compiler_dispose(&c);
  return err;
}


/*
static int buildfile_cmp(const buildfile_t* x, const buildfile_t* y, void* nullable _) {
  int d = memcmp(x->ofile.p, y->ofile.p, MIN(x->ofile.len, y->ofile.len));
  return d != 0 ? d :
         x->ofile.len < y->ofile.len ? 1 :
         y->ofile.len < x->ofile.len ? -1 :
         0;
}


static err_t pkg_checksum_src(compiler_t* c, buildfile_t* fv, u32 fc, u8 result[32]) {
  SHA256 state;
  sha256_init(&state, result);

  co_qsort(fv, fc, sizeof(*fv), (co_qsort_cmp)buildfile_cmp, NULL);
  for (u32 i = 0; i < fc; i++)
    sha256_write(&state, fv[i].input->sha256, 32);

  sha256_close(&state);

  #if DEBUG
    buf_t b = buf_make(c->ma);
    buf_appendhex(&b, result, 32);
    if (buf_nullterm(&b))
      dlog("result: %s", b.chars);
    buf_dispose(&b);
  #endif

  return ErrCanceled; // XXX
}*/
