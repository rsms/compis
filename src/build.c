// SPDX-License-Identifier: Apache-2.0
#include "colib.h"
#include "path.h"
#include "compiler.h"
#include "subproc.h"
#include "bgtask.h"
#include "dirwalk.h"
#include "thread.h"
#include "threadpool.h"
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
static int opt_verbose = 0;
static const char* opt_maxproc = "";
static bool opt_printast = false;
static bool opt_printir = false;
static bool opt_genirdot = false;
static bool opt_genasm = false;
static bool opt_nolink = false;
static bool opt_nomain = false;
static bool opt_nostdruntime = false;
static bool opt_version = false;
static const char* opt_builddir = "build";
#if DEBUG
  static bool opt_trace_all = false;
  bool opt_trace_scan = false;
  bool opt_trace_parse = false;
  bool opt_trace_typecheck = false;
  bool opt_trace_comptime = false;
  bool opt_trace_import = false;
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
  LV(&opt_targetstr,    "target", "<target>", "Build for <target> instead of host")\
  LV(&opt_builddir,     "build-dir", "<dir>", "Use <dir> instead of ./build")\
  L( &opt_printast,     "print-ast",          "Print AST to stderr")\
  L( &opt_printir,      "print-ir",           "Print IR to stderr")\
  L( &opt_genirdot,     "write-ir-dot",       "Write IR as Graphviz .dot file to build dir")\
  L( &opt_nolink,       "no-link",            "Only compile, don't link")\
  L( &opt_nomain,       "no-main",            "Don't auto-generate C ABI \"main\" for main.main")\
  L( &opt_nostdruntime, "no-stdruntime",      "Don't automatically import std/runtime")\
  L( &opt_version,      "version",            "Print Compis version on stdout and exit")\
  /* debug-only options */\
  DEBUG_L( &opt_trace_all,       "trace",           "Trace everything")\
  DEBUG_L( &opt_trace_scan,      "trace-scan",      "Trace lexical scanning")\
  DEBUG_L( &opt_trace_parse,     "trace-parse",     "Trace parsing")\
  DEBUG_L( &opt_trace_typecheck, "trace-typecheck", "Trace type checking")\
  DEBUG_L( &opt_trace_comptime,  "trace-comptime",  "Trace comptime eval")\
  DEBUG_L( &opt_trace_import,    "trace-import",    "Trace importing of packages")\
  DEBUG_L( &opt_trace_ir,        "trace-ir",        "Trace IR")\
  DEBUG_L( &opt_trace_cgen,      "trace-cgen",      "Trace code generation")\
  DEBUG_L( &opt_trace_subproc,   "trace-subproc",   "Trace subprocess execution")\
// end FOREACH_CLI_OPTION

#include "cliopt.inc.h"

static void help(const char* prog) {
  printf(
    "Compis " CO_VERSION_STR ", your friendly neighborhood compiler\n"
    "Usage: %s %s [options] [--] [<sourcedir>]\n"
    "       %s %s [options] [--] <package>\n"
    "       %s %s [options] [--] <sourcefile> ...\n"
    "Options:\n"
    "",
    coprogname, prog,
    coprogname, prog,
    coprogname, prog);
  cliopt_print();
  exit(0);
}


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


static void vlog_config(const compiler_t* c) {
  printf("COMAXPROC: %u\n", comaxproc);
  printf("COROOT:    %s\n", coroot);
  printf("COCACHE:   %s\n", cocachedir);
  printf("COPATH:    ");
  for (const char*const* p = copath; *p; p++)
    printf("%s%s", p == copath ? "" : ":", *p);
  printf("\n");

  char tmpbuf[TARGET_FMT_BUFCAP];
  target_fmt(opt_target, tmpbuf, sizeof(tmpbuf));
  printf("target:    %s (%s)\n", tmpbuf, opt_target->triple);

  if (coverbose == 1) {
    printf("(-vv for more details)\n");
    return;
  }

  printf("sysroot:   %s\n", c->sysroot);

  printf("buildmode: ");
  switch ((enum buildmode)c->buildmode) {
    case BUILDMODE_DEBUG: printf("debug\n"); break;
    case BUILDMODE_OPT:   printf("opt\n"); break;
  }

  printf("buildroot: %s\n", c->buildroot);
  printf("builddir:  %s\n", c->builddir);
  printf("ldname:    %s\n", c->ldname);
  printf("lto:       %s\n", c->lto ? "enabled" : "disabled");
  printf("addrtype:  %s\n", primtype_name(c->addrtype->kind));
  printf("uinttype:  %s\n", primtype_name(c->uinttype->kind));
  printf("inttype:   %s\n", primtype_name(c->inttype->kind));
}


int main_build(int argc, char* argv[]) {
  if (!cliopt_parse(&argc, &argv, help))
    return 1;

  coverbose = MAX(coverbose, (u8)opt_verbose);

  #if DEBUG
    // --co-trace turns on all trace flags
    opt_trace_scan |= opt_trace_all;
    opt_trace_parse |= opt_trace_all;
    opt_trace_typecheck |= opt_trace_all;
    opt_trace_comptime |= opt_trace_all;
    opt_trace_import |= opt_trace_all;
    opt_trace_ir |= opt_trace_all;
    opt_trace_cgen |= opt_trace_all;
    opt_trace_subproc |= opt_trace_all;
  #endif

  if (opt_version) {
    print_co_version();
    return 0;
  }

  // if (optind == argc)
  //   errx(1, "no input (see %s %s --help)", coprogname, argv[0]);

  if (*opt_maxproc)
    set_comaxproc();

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

  // decide what package to build
  pkg_t* pkgv;
  u32 pkgc;
  err_t err = 0;
  if (( err = pkgs_for_argv(argc, (const char*const*)argv, &pkgv, &pkgc) ))
    return 1;
  assert(pkgc > 0);

  // -o <path> makes no sense when building multiple packages
  if (pkgc > 1 && *opt_out)
    errx(1, "cannot specify -o option when building multiple packages");

  // initialize thread pool
  if (( err = threadpool_init() ))
    elog("failed to initialize thread pool: %s", err_str(err));

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
    .nostdruntime = opt_nostdruntime,
  };
  if (err || ( err = compiler_configure(&c, &ccfg) )) {
    dlog("compiler_configure: %s", err_str(err));
    return 1;
  }

  if (coverbose)
    vlog_config(&c);

  // build sysroot if needed (only reads compiler attributes; never mutates it)
  if (( err = build_sysroot(&c, /*flags*/0) )) {
    dlog("build_sysroot: %s", err_str(err));
    return 1;
  }

  // build packages
  u32 pkgbuild_flags = 0;
  if (opt_nolink) pkgbuild_flags |= PKGBUILD_NOLINK;
  pkgbuild_flags |= PKGBUILD_NOCLEANUP; // since we exit the process after this

  for (u32 i = 0; i < pkgc; i++) {
    pkg_t* pkg = &pkgv[i];
    if (( err = pkgindex_add(&c, pkg) )) {
      dlog("pkgindex_add(pkg_t{dir=\"%s\"}) failed: %s", pkg->dir.p, err_str(err));
      break;
    }
    if (( err = build_toplevel_pkg(pkg, &c, opt_out, pkgbuild_flags) )) {
      dlog("error while building pkg %s: %s", pkg->path.p, err_str(err));
      break;
    }
  }

  // compiler_dispose(&c); // would need to do this if we didn't just exit
  return (int)!!err;
}
