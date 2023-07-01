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
#include "pkgbuild.h"

#include <stdlib.h> // exit
#include <unistd.h> // getopt
#include <string.h> // strdup
#include <err.h>
#include <errno.h>
#include <getopt.h>

// cli options
static bool opt_help = false;
static const char* opt_out = "";
static const char* opt_targetstr = "";
static const target_t* opt_target = NULL;
static bool opt_debug = false;
static bool opt_verbose = false;
static bool opt_vverbose = false;
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
  SV(&opt_out,    'o', "out","<file>",  "Write product to <file> instead of build dir")\
  S( &opt_debug,  'd', "debug",         "Build in debug aka development mode")\
  S( &opt_verbose,'v', "verbose",       "Verbose mode prints extra information")\
  SV(&opt_maxproc,'j', "maxproc","<N>", "Use up to N parallel processes/threads")\
  S( &opt_genasm, 'S', "write-asm",     "Write machine assembly sources to build dir")\
  S( &opt_help,   'h', "help",          "Print help on stdout and exit")\
  /* advanced options (long form only) */ \
  LV(&opt_targetstr,"target", "<target>", "Build for <target> instead of host")\
  LV(&opt_builddir, "build-dir", "<dir>", "Use <dir> instead of ./build")\
  L( &opt_printast, "print-ast",    "Print AST to stderr")\
  L( &opt_printir,  "print-ir",     "Print IR to stderr")\
  L( &opt_genirdot, "write-ir-dot", "Write IR as Graphviz .dot file to build dir")\
  L( &opt_logld,    "print-ld-cmd", "Print linker invocation to stderr")\
  L( &opt_nolink,   "no-link",      "Only compile, don't link")\
  L( &opt_nomain,   "no-auto-main", "Don't auto-generate C ABI \"main\" for main.main")\
  L( &opt_vverbose, "vv",           "Extra verbose mode") \
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


static err_t build_pkg(
  pkg_t* pkg, compiler_t* c, const char* outfile, u32 pkgbuild_flags);


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


static void diaghandler(const diag_t* d, void* nullable userdata) {
  // TODO: send over chan_t when building in parallel
  elog("%s", d->msg);
  if (d->srclines && *d->srclines)
    elog("%s", d->srclines);
}


int main_build(int argc, char* argv[]) {
  int optind = parse_cli_options(argc, argv, help);
  if (optind < 0)
    return 1;

  coverbose = MAX(coverbose, (u8)opt_verbose + (u8)opt_vverbose);
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
    errx(1, "no input (see %s %s --help)", coprogname, argv[0]);

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
  assert(pkgc > 0);

  // // for now we limit ourselves to building one package per compis invocation
  // if (pkgc > 1) {
  //   errx(1, "more than one package requested; please only specify one package");
  // }
  if (pkgc > 1 && *opt_out)
    errx(1, "cannot specify -o option when building multiple packages");

  // create a compiler instance
  compiler_t c;
  compiler_init(&c, memalloc_ctx(), &diaghandler);

  // configure compiler
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
  if (err || ( err = compiler_configure(&c, &ccfg) )) {
    dlog("compiler_configure: %s", err_str(err));
    return 1;
  }

  // build sysroot if needed (only reads compiler attributes; never mutates it)
  if (( err = build_sysroot_if_needed(&c, /*flags*/0) )) {
    dlog("build_sysroot_if_needed: %s", err_str(err));
    return 1;
  }

  // // XXX FIXME
  // if (!streq(pkgv[0].name.p, "std/runtime")) {
  //   if (( err = build_dep_pkg(str_make("std/runtime"), &ccfg) ))
  //     return 1;
  // }

  // build packages
  u32 pkgbuild_flags = 0
                     | (opt_nolink ? PKGBUILD_NOLINK : 0)
                     ;
  for (u32 i = 0; i < pkgc; i++) {
    pkg_t* pkg = &pkgv[i];

    if (pkg_is_built(pkg, &c)) {
      log("[%s] up to date", pkg->name.p); // in lieu of bgtask
      continue;
    }

    if (( err = build_pkg(pkg, &c, opt_out, pkgbuild_flags) )) {
      dlog("error while building pkg %s: %s", pkg->name.p, err_str(err));
      break;
    }
  }

  // compiler_dispose(&c); // would need to do this if we didn't just exit
  return (int)!!err;
}


static err_t build_pkg(
  pkg_t* pkg, compiler_t* c, const char* outfile, u32 pkgbuild_flags)
{
  err_t err;
  c->errcount = 0;

  pkgbuild_t pb;
  if (( err = pkgbuild_init(&pb, pkg, c, pkgbuild_flags) ))
    return err;

  // locate source files
  if (( err = pkgbuild_locate_sources(&pb) )) {
    dlog("pkgbuild_locate_sources: %s", err_str(err));
    goto end;
  }

  // begin compilation of C source files
  if (( err = pkgbuild_begin_early_compilation(&pb) )) {
    dlog("pkgbuild_begin_early_compilation: %s", err_str(err));
    goto end;
  }

  // parse source files
  if (( err = pkgbuild_parse(&pb) )) {
    dlog("pkgbuild_parse: %s", err_str(err));
    goto end;
  }

  // typecheck package
  if (( err = pkgbuild_typecheck(&pb) )) {
    dlog("pkgbuild_typecheck: %s", err_str(err));
    goto end;
  }

  // generate C code for package
  if (( err = pkgbuild_cgen(&pb) )) {
    dlog("pkgbuild_cgen: %s", err_str(err));
    goto end;
  }

  // begin compilation of C source files generated from compis sources
  if (( err = pkgbuild_begin_late_compilation(&pb) )) {
    dlog("pkgbuild_begin_late_compilation: %s", err_str(err));
    goto end;
  }

  // wait for compilation tasks to finish
  if (( err = pkgbuild_await_compilation(&pb) )) {
    dlog("pkgbuild_await_compilation: %s", err_str(err));
    goto end;
  }

  // link exe or library (does nothing if PKGBUILD_NOLINK flag is set)
  if (( err = pkgbuild_link(&pb, outfile) )) {
    dlog("pkgbuild_link: %s", err_str(err));
    goto end;
  }

end:
  pkgbuild_dispose(&pb); // TODO: can skip this for top-level package
  return err;
}


// static err_t build_dep_pkg(str_t pkgname, const compiler_config_t* parent_ccfg) {
//   pkg_t pkg = {
//     .name = pkgname,
//   };
//   if (!pkg_find_dir(&pkg)) {
//     elog("package not found: %s", pkgname.p);
//     return ErrNotFound;
//   }
//
//   compiler_config_t ccfg = *parent_ccfg;
//   ccfg.printast = false;
//   ccfg.printir = false;
//   ccfg.genirdot = false;
//   ccfg.genasm = false;
//   ccfg.nomain = false;
//
//   err_t err = build_pkg(&pkg, &ccfg, "", /*flags*/0);
//   if (err)
//     dlog("error while building pkg %s: %s", pkg.name.p, err_str(err));
//
//   return err;
// }


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
