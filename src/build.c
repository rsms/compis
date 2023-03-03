// SPDX-License-Identifier: Apache-2.0
#include "colib.h"
#include "path.h"
#include "compiler.h"
#include "subproc.h"
#include "bgtask.h"

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
    "Usage: co %s [options] [--] <source> ...\n"
    "Options:\n"
    "",
    prog);
  print_options();
  exit(0);
}


static err_t build_exe(char*const* srcfilev, usize filecount);


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

  if (opt_version) {
    print_co_version();
    return 0;
  }

  if (optind == argc)
    errx(1, "missing input source");

  if (*opt_maxproc)
    set_comaxproc();

  assert(optind <= argc);
  argv += optind;
  argc -= optind;

  if (!( opt_target = target_find(opt_targetstr) )) {
    log("Invalid target \"%s\"", opt_targetstr);
    log("See `%s targets` for a list of supported targets", relpath(coexefile));
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


typedef struct {
  input_t*  input;
  buf_t     ofile;
  promise_t promise;
} buildfile_t;


// build_syslibs.c
err_t build_syslibs_if_needed(compiler_t* c);


static err_t link_exe(
  compiler_t* c, bgtask_t* task, const char* outfile,
  buildfile_t* infilev, usize infilec)
{
  // TODO: -Llibdir
  // char libflag[PATH_MAX];
  // snprintf(libflag, sizeof(libflag), "-L%s", c->libdir);

  CoLLVMLink link = {
    .target_triple = c->target.triple,
    .outfile = outfile,
    .infilec = infilec,
    .sysroot = c->sysroot,
    .print_lld_args = opt_verbose || opt_logld,
    .lto_level = c->buildmode == BUILDMODE_DEBUG ? 0 : 2,
    .lto_cachedir = "",
  };

  err_t err = 0;

  if (link.lto_level > 0)
    link.lto_cachedir = path_join_alloca(c->builddir, "llvm");

  // linker wants an array of cstring pointers
  link.infilev = mem_alloctv(c->ma, const char*, infilec);
  if (!link.infilev) {
    err = ErrNoMem;
    goto end;
  }
  for (usize i = 0; i < infilec; i++)
    link.infilev[i] = infilev[i].ofile.chars;

  task->n++;
  bgtask_setstatusf(task, "link %s", relpath(outfile));
  err = llvm_link(&link);

  mem_freetv(c->ma, link.infilev, infilec);

end:
  return err;
}


static err_t build_exe(char*const* srcfilev, usize filecount) {
  err_t err = 0;

  if (filecount == 0)
    return ErrInvalid;

  compiler_t c;
  compiler_init(&c, memalloc_ctx(), &diaghandler, "main"); // FIXME pkgname
  c.opt_printast = opt_printast;
  c.opt_printir = opt_printir;
  c.opt_genirdot = opt_genirdot;
  c.opt_genasm = opt_genasm;
  c.opt_verbose = opt_verbose;
  c.nomain = opt_nomain;
  c.buildmode = opt_debug ? BUILDMODE_DEBUG : BUILDMODE_OPT;
  if (err || ( err = compiler_configure(&c, opt_target, opt_builddir) )) {
    dlog("compiler_configure: %s", err_str(err));
    compiler_dispose(&c);
    return err;
  }

  // build system libraries, if needed
  if (!opt_nolink && (err = build_syslibs_if_needed(&c)))
    return err;

  // create output dir
  if (( err = fs_mkdirs(c.pkgbuilddir, 0770) ))
    goto end;

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

  const char* outfile;
  if (*opt_out) {
    outfile = opt_out;
  } else {
    outfile = path_join_alloca(c.builddir, c.pkgname);
  }

  int taskflags = c.opt_verbose ? BGTASK_NOFANCY : 0;
  u32 tasklen = (u32)filecount + !opt_nolink;
  bgtask_t* task = bgtask_start(c.ma, c.pkgname, tasklen, taskflags);

  // compile object files
  for (usize i = 0; i < filecount; i++) {
    task->n++;
    bgtask_setstatusf(task, "compile %s", relpath(fv[i].input->name));
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
    err = link_exe(&c, task, outfile, fv, filecount);

  bgtask_end(task);
  task = NULL;

end:
  for (usize i = 0; i < filecount; i++)
    input_free(fv[i].input, c.ma);
  mem_freetv(c.ma, fv, filecount);
  compiler_dispose(&c);
  if (task)
    bgtask_end(task);
  return err;
}
