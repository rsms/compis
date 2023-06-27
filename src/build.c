// SPDX-License-Identifier: Apache-2.0
#include "colib.h"
#include "path.h"
#include "compiler.h"
#include "subproc.h"
#include "bgtask.h"
#include "dirwalk.h"
#include "sha256.h"

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

  // XXX FIXME
  if (!streq(pkgv[0].name.p, "std/runtime")) {
    if (( err = build_dep_pkg(str_make("std/runtime"), &ccfg) ))
      return 1;
  }

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


static int cstr_cmp(const char** a, const char** b, void* ctx) {
  return strcmp(*a, *b);
}


static err_t mkdirs_for_files(memalloc_t ma, const char*const* filev, u32 filec) {
  err_t err = 0;
  char dir[PATH_MAX];
  ptrarray_t dirs = {0};

  for (u32 i = 0; i < filec; i++) {
    // e.g. "/foo/bar/cat.lol" => "/foo/bar"
    usize dirlen = path_dir_buf(dir, sizeof(dir), filev[i]);
    if (dirlen >= sizeof(dir)) {
      err = ErrOverflow;
      break;
    }

    const char** vp = array_sortedset_assign(
      const char*, &dirs, ma, &dir, (array_sorted_cmp_t)cstr_cmp, NULL);
    if UNLIKELY(!vp) {
      err = ErrNoMem;
      break;
    }
    if (*vp == NULL) {
      *vp = mem_strdup(ma, (slice_t){.p=dir,.len=dirlen}, 0);
      if UNLIKELY(*vp == NULL) {
        err = ErrNoMem;
        break;
      }
    }
  }

  if (!err) for (u32 i = 0; i < dirs.len; i++) {
    const char* dir = (const char*)dirs.v[i];
    if (( err = fs_mkdirs(dir, 0755, 0) ))
      break;
  }

  ptrarray_dispose(&dirs, ma);
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

  // allocate array for promises
  promise_t* promisev = mem_alloctv(c.ma, promise_t, (usize)pkg->files.len);
  safecheckf(promisev, "out of memory");

  // create list of object files
  strlist_t objfiles;
  char*const* objfilev = pkg_mkobjfilelist(pkg, &c, &objfiles);

  // create objfile directories, if needed.
  // note: only "main" package may have srcfiles with subdirectories.
  if (streq(pkg->name.p, "main")) {
    err = mkdirs_for_files(c.ma, (const char*const*)objfilev, (usize)pkg->files.len);
    if (err)
      goto end2;
  }

  // configure a bgtask
  int taskflags = c.opt_verbose ? BGTASK_NOFANCY : 0;
  u32 tasklen = pkg->files.len + (u32)!(flags & BUILD_NOLINK);
  bgtask_t* task = bgtask_start(c.ma, pkg->name.p, tasklen, taskflags);

  // compile source files
  for (u32 i = 0; i < pkg->files.len; i++) {
    srcfile_t* sf = &pkg->files.v[i];
    task->n++;
    bgtask_setstatusf(task, "compile %s", relpath(sf->name.p));
    if (( err = compiler_compile(&c, &promisev[i], sf, objfilev[i]) ))
      break;
  }

  // wait for all compiler processes
  for (u32 i = 0; i < pkg->files.len; i++) {
    err_t err1 = promise_await(&promisev[i]);
    if (!err)
      err = err1;
  }

  // create executable if there's a main function
  if (!(flags & BUILD_NOLINK) && c.mainfun) {
    if (!*outfile)
      outfile = path_join_alloca(c.builddir, c.pkg->name.p);
    err = link_exe(&c, task, outfile, objfilev, pkg->files.len);
  } else if (!(flags & BUILD_NOLINK)) {
    // create static library
    if (!*outfile)
      outfile = path_join_alloca(c.builddir, strcat_alloca(c.pkg->name.p, ".a"));
    err = archive_lib(&c, task, outfile, objfilev, pkg->files.len);
  }

  // cleanup object files
  if (!err) for (u32 i = 0; i < pkg->files.len; i++) {
    if (unlink(objfilev[i]) != 0)
      elog("warning: failed to remove %s: %s", objfilev[i], err_str(err_errno()));
  }

  // end bgtask
  bgtask_end(task, "%s", (flags & BUILD_NOLINK) ? "(compile only)" : relpath(outfile));

end2:
  strlist_dispose(&objfiles);

  mem_freetv(c.ma, promisev, (usize)pkg->files.len);

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
