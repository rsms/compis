// SPDX-License-Identifier: Apache-2.0
#include "colib.h"
#include "path.h"
#include "compiler.h"
#include "subproc.h"

#include <stdlib.h> // exit
#include <unistd.h> // getopt
#include <string.h> // strdup
#include <sys/stat.h>
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
    "Compis, your friendly neighborhood compiler\n"
    "Usage: co %s [options] [--] <source> ...\n"
    "Options:\n"
    "",
    prog);
  print_options();
  exit(0);
}


static err_t build_exe(char*const* srcfilev, usize filecount);


bool select_target() {
  const char* targetstr = "";
  targetstr = "aarch64-linux";
  const target_t* target = target_find(targetstr);
  if (!target) {
    log("Invalid target \"%s\"", targetstr);
    log("See `%s targets` for a list of supported targets.", coexefile);
    return false;
  }
  #if DEBUG
  {
    char tmpbuf[64];
    target_fmt(target, tmpbuf, sizeof(tmpbuf));
    dlog("targeting %s", tmpbuf);
  }
  #endif
  return true;
}


int main_build(int argc, char* argv[]) {
  memalloc_t ma = memalloc_ctx();

  tmpbuf_init(ma);
  sym_init(ma);

  int optind = parse_cli_options(argc, argv, help);
  if (optind < 0)
    return 1;

  #if DEBUG
    // --co-trace turns on all trace flags
    opt_trace_parse |= opt_trace_all;
    opt_trace_typecheck |= opt_trace_all;
    opt_trace_comptime |= opt_trace_all;
    opt_trace_ir |= opt_trace_all;
    opt_trace_cgen |= opt_trace_all;
    opt_trace_subproc |= opt_trace_all;
  #endif

  if (optind == argc)
    errx(1, "missing input source");

  if (*opt_maxproc) {
    char* end;
    unsigned long n = strtoul(opt_maxproc, &end, 10);
    if (n == ULONG_MAX || n > U32_MAX || *end || (n == 0 && errno))
      errx(1, "invalid value for -j: %s", opt_maxproc);
    if (n != 0) {
      comaxproc = (u32)n;
      dlog("setting comaxproc=%u from -j option", comaxproc);
    }
  }

  assert(optind <= argc);
  argv += optind;
  argc -= optind;

  if (!( opt_target = target_find(opt_targetstr) )) {
    log("Invalid target \"%s\"", opt_targetstr);
    log("See `%s targets` for a list of supported targets", coexefile);
    return 1;
  }
  #if DEBUG
    char tmpbuf[64];
    target_fmt(opt_target, tmpbuf, sizeof(tmpbuf));
    dlog("targeting %s (%s)", tmpbuf, opt_target->triple);
  #endif

  if (opt_nolink && *opt_out)
    errx(1, "cannot specify both --no-link and -o (nothing to output when not linking)");

  err_t err = build_exe(argv, (usize)argc);
  if (err && err != ErrCanceled)
    dlog("failed to build: %s", err_str(err));

  return err == 0 ? 0 : 1;
}


static void diaghandler(const diag_t* d, void* nullable userdata) {
  log("%s", d->msg);
  if (d->srclines && *d->srclines)
    log("%s", d->srclines);
}


static input_t* open_input(memalloc_t ma, const char* filename) {
  input_t* input = input_create(ma, filename);
  if (input == NULL)
    panic("out of memory");
  err_t err;
  if ((err = input_open(input)))
    errx(1, "%s: %s", filename, err_str(err));
  return input;
}


static char* nullable make_output_file(compiler_t* c) {
  return path_join(c->ma, c->builddir, c->pkgname);
}


static char* nullable make_lto_cachedir(compiler_t* c) {
  return path_join(c->ma, c->builddir, "llvm");
}


typedef struct {
  input_t*  input;
  buf_t     ofile;
  promise_t promise;
} buildfile_t;


// build_syslibs.c
err_t build_syslibs_if_needed(compiler_t* c);


static err_t link_exe(compiler_t* c, buildfile_t* buildfilev, usize buildfilec) {
  err_t err = build_syslibs_if_needed(c);
  if (err)
    return err;

  const char* outfile = *opt_out ? opt_out : make_output_file(c);

  // TODO: -Llibdir
  // char libflag[PATH_MAX];
  // snprintf(libflag, sizeof(libflag), "-L%s", c->libdir);

  CoLLVMLink link = {
    .target_triple = c->target.triple,
    .outfile = outfile,
    .infilec = buildfilec,
    .sysroot = c->sysroot,
    .print_lld_args = opt_verbose || opt_logld,
    .lto_level = c->buildmode == BUILDMODE_DEBUG ? 0 : 2,
    .lto_cachedir = "",
  };

  char* lto_cachedir = NULL;
  if (link.lto_level > 0) {
    char* lto_cachedir = make_lto_cachedir(c);
    if (!outfile || !lto_cachedir) {
      err = ErrNoMem;
      goto end;
    }
    link.lto_cachedir = lto_cachedir;
  }

  // linker wants an array of cstring pointers
  link.infilev = mem_alloctv(c->ma, const char*, buildfilec);
  if (!link.infilev) {
    err = ErrNoMem;
    goto end;
  }
  for (usize i = 0; i < buildfilec; i++)
    link.infilev[i] = buildfilev[i].ofile.chars;

  log("link %s", outfile);
  err = llvm_link(&link);

  mem_freetv(c->ma, link.infilev, buildfilec);

end:
  if (outfile && outfile != opt_out)
    mem_freecstr(c->ma, (char*)outfile);
  mem_freecstr(c->ma, lto_cachedir);
  return err;
}


static err_t build_exe(char*const* srcfilev, usize filecount) {
  err_t err = 0;

  if (filecount == 0)
    return ErrInvalid;

  slice_t builddir = slice_cstr(opt_builddir);
  slice_t pkgname = slice_cstr("main"); // TODO FIXME

  compiler_t c;
  compiler_init(&c, memalloc_ctx(), &diaghandler, pkgname);
  c.opt_printast = opt_printast;
  c.opt_printir = opt_printir;
  c.opt_genirdot = opt_genirdot;
  c.opt_genasm = opt_genasm;
  c.opt_verbose = opt_verbose;
  c.nomain = opt_nomain;
  c.buildmode = opt_debug ? BUILDMODE_DEBUG : BUILDMODE_OPT;
  if (err || ( err = compiler_configure(&c, opt_target, builddir) )) {
    compiler_dispose(&c);
    return err;
  }

  // fv is an array of files we are building
  buildfile_t* fv = mem_alloctv(c.ma, buildfile_t, filecount);
  if (!fv) {
    compiler_dispose(&c);
    return ErrNoMem;
  }
  for (usize i = 0; i < filecount; i++) {
    fv[i].input = open_input(c.ma, srcfilev[i]);
    buf_init(&fv[i].ofile, c.ma);
  }

  // create output dir
  if (( err = fs_mkdirs(c.pkgbuilddir, strlen(c.pkgbuilddir), 0770) ))
    goto end;

  // compile object files
  for (usize i = 0; i < filecount; i++) {
    log("compile %s", fv[i].input->name);
    if (( err = compiler_compile(&c, &fv[i].promise, fv[i].input, &fv[i].ofile) ))
      break;
  }

  // wait for all compiler processes
  for (usize i = 0; i < filecount; i++) {
    err_t err1 = promise_await(&fv[i].promise);
    if (!err)
      err = err1;
  }

  // link executable
  if (!err && !opt_nolink)
    err = link_exe(&c, fv, filecount);

end:
  for (usize i = 0; i < filecount; i++)
    input_free(fv[i].input, c.ma);
  mem_freetv(c.ma, fv, filecount);
  compiler_dispose(&c);
  return err;
}
