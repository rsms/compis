// SPDX-License-Identifier: Apache-2.0
#include "colib.h"
#include "path.h"
#include "compiler.h"

#include <stdlib.h> // exit
#include <unistd.h> // getopt
#include <string.h> // strdup
#include <err.h>
#include <getopt.h>

#include "llvm/llvm.h"


const char* COROOT = ""; // Directory of co itself; dirname(argv[0])

extern CoLLVMOS host_os; // defined in main.c

// cli options
static bool opt_help = false;
static const char* opt_out = "";
static bool opt_debug = false;
static bool opt_printast = false;
static bool opt_printir = false;
static bool opt_genirdot = false;
static bool opt_genasm = false;
static bool opt_logld = false;
static bool opt_nomain = false;
static const char* opt_builddir = "build";

#define FOREACH_CLI_OPTION(S, SV, L, LV) \
  /* S( var, ch, name,          descr) */\
  /* SV(var, ch, name, valname, descr) */\
  /* L( var,     name,          descr) */\
  /* LV(var,     name, valname, descr) */\
  SV(&opt_out,    'o', "out", "<file>", "Write product to <file> instead of build dir")\
  S( &opt_debug,  'd', "debug",     "Build in debug aka development mode")\
  S( &opt_genasm, 'S', "write-asm", "Write machine assembly sources to build dir")\
  S( &opt_help,   'h', "help",      "Print help on stdout and exit")\
  /* advanced options (long form only) */ \
  LV(&opt_builddir, "build-dir", "<dir>", "Use <dir> instead of ./build")\
  L( &opt_printast, "print-ast",    "Print AST to stderr")\
  L( &opt_printir,  "print-ir",     "Print IR to stderr")\
  L( &opt_genirdot, "write-ir-dot", "Write IR as Graphviz .dot file to build dir")\
  L( &opt_logld,    "print-ld-cmd", "Print linker invocation to stderr")\
  L( &opt_nomain,   "no-auto-main", "Don't auto-generate C ABI \"main\" for main.main")\
// end FOREACH_CLI_OPTION

#include "cliopt.inc.h"

static void help(const char* prog) {
  printf(
    "Compis, your friendly neighborhood compiler\n"
    "usage: co %s [options] [--] <source> ...\n"
    "options:\n"
    "",
    prog);
  print_options();
  exit(0);
}


static err_t build_exe(char*const* srcfilev, usize filecount);


int main_build(int argc, char* argv[]) {
  char* coroot = LLVMGetMainExecutable(argv[0]);
  coroot[path_dirlen(coroot, strlen(coroot))] = 0;
  COROOT = coroot;

  int optind = parse_cli_options(argc, argv, help);
  if (optind < 0)
    return 1;

  if (optind == argc)
    errx(1, "missing input source");

  assert(optind <= argc);
  argv += optind;
  argc -= optind;

  sym_init(memalloc_ctx());

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


static char* nullable path_join(memalloc_t ma, slice_t left, slice_t right) {
  char* s = mem_strdup(ma, left, 1 + right.len);
  if (!s)
    return NULL;
  char* p = s + left.len;
  *p++ = PATH_SEPARATOR;
  memcpy(p, right.p, right.len);
  p[right.len] = 0;
  return s;
}


static char* nullable make_output_file(compiler_t* c) {
  return path_join(c->ma, slice_cstr(c->builddir), slice_cstr(c->pkgname));
}


static char* nullable make_lto_cachedir(compiler_t* c) {
  return path_join(c->ma, slice_cstr(c->builddir), slice_cstr("llvm"));
}


typedef struct {
  input_t*  input;
  buf_t     ofile;
  promise_t promise;
} buildfile_t;


static err_t link_exe(compiler_t* c, buildfile_t* buildfilev, usize buildfilec) {
  err_t err = 0;

  const char* outfile = *opt_out ? opt_out : make_output_file(c);
  char* lto_cachedir = make_lto_cachedir(c);
  if (!outfile || !lto_cachedir) {
    err = ErrNoMem;
    goto end;
  }

  CoLLVMLink link = {
    .target_triple = c->triple,
    .outfile = outfile,
    .infilec = buildfilec,
    .print_lld_args = opt_logld,
    .lto_level = c->buildmode == BUILDMODE_DEBUG ? 0 : 2,
    .lto_cachedir = lto_cachedir,
  };

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
    mem_freex(c->ma, MEM((void*)outfile, strlen(outfile)));
  if (lto_cachedir)
    mem_freex(c->ma, MEM(lto_cachedir, strlen(lto_cachedir)));
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
  c.nomain = opt_nomain;
  c.buildmode = opt_debug ? BUILDMODE_DEBUG : BUILDMODE_OPT;
  const char* target_triple = llvm_host_triple();
  // const char* target_triple = "aarch64-linux-unknown";
  if (( err = compiler_configure(&c, target_triple, builddir) )) {
    compiler_dispose(&c);
    return err;
  }
  dlog("target: %s", c.triple);

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
  if (!err)
    err = link_exe(&c, fv, filecount);

end:
  for (usize i = 0; i < filecount; i++)
    input_free(fv[i].input, c.ma);
  mem_freetv(c.ma, fv, filecount);
  compiler_dispose(&c);
  return err;
}
