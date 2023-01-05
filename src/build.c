// SPDX-License-Identifier: Apache-2.0
#include "colib.h"
#include "path.h"
#include "compiler.h"

#include <stdlib.h> // exit
#include <unistd.h> // getopt
#include <string.h> // strdup
#include <err.h>

#include "llvm/llvm.h"


const char* COROOT = ""; // Directory of co itself; dirname(argv[0])

extern CoLLVMOS host_os; // defined in main.c

// cli options
static const char* opt_outfile = "a.out";
static bool opt_printast = false;
static bool opt_printir = false;
static bool opt_genirdot = false;
static bool opt_genasm = false;
static bool opt_logld = false;
static bool opt_nomain = false;


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


static err_t build_exe(const char** srcfilev, usize filecount) {
  err_t err = 0;

  if (filecount == 0)
    return ErrInvalid;

  const char* pkgname = "main"; // TODO FIXME

  compiler_t c;
  compiler_init(&c, memalloc_ctx(), &diaghandler, slice_cstr(pkgname));
  c.opt_printast = opt_printast;
  c.opt_printir = opt_printir;
  c.opt_genirdot = opt_genirdot;
  c.opt_genasm = opt_genasm;
  c.nomain = opt_nomain;
  // compiler_set_triple(&c, "aarch64-linux-unknown");
  dlog("compiler.triple: %s", c.triple);

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

  // create object dir
  if (( err = fs_mkdirs(c.objdir, strlen(c.objdir), 0770) ))
    goto end;

  // compile object files
  for (usize i = 0; i < filecount; i++) {
    log("compile %s", fv[i].input->name);
    if (( err = compiler_compile(&c, &fv[i].promise, fv[i].input, &fv[i].ofile) ))
      break;
  }

  // wait for all compiler processes
  for (usize i = 0; i < filecount; i++) {
    if (!promise_isresolved(&fv[i].promise)) {
      err_t err1 = promise_await(&fv[i].promise);
      if (err1 && !err) err = err1;
    }
  }

  if (err)
    goto end;

  // link executable
  log("link %s", opt_outfile);
  CoLLVMLink link = {
    .target_triple = c.triple,
    .outfile = opt_outfile,
    .infilec = filecount,
    .print_lld_args = opt_logld,
  };
  // (linker wants an array of cstring pointers)
  link.infilev = mem_alloctv(c.ma, const char*, filecount);
  if (!link.infilev) {
    err = ErrNoMem;
  } else {
    for (usize i = 0; i < filecount; i++)
      link.infilev[i] = fv[i].ofile.chars;
    err = llvm_link(&link);
    mem_freetv(c.ma, link.infilev, filecount);
  }

end:
  for (usize i = 0; i < filecount; i++)
    input_free(fv[i].input, c.ma);
  mem_freetv(c.ma, fv, filecount);
  compiler_dispose(&c);
  return err;
}


static void usage(const char* prog) {
  printf(
    "Compis, your friendly neighborhood compiler\n"
    "usage: co %s [options] <source> ...\n"
    "options:\n"
    "  -o <file>  Write executable to <file> instead of %s\n"
    "  -A         Print AST to stderr\n"
    "  -R         Print IR SSA to stderr\n"
    "  -D         Write IR SSA as Graphviz .dot file\n"
    "  -S         Write machine assembly sources to build dir\n"
    "  -K         Print linker invocation to stderr\n"
    "  -h         Print help on stdout and exit\n"
    "",
    prog,
    opt_outfile);
}


static int parse_cli_options(int argc, const char** argv) {
  extern char* optarg; // global state in libc... coolcoolcool
  extern int optind, optopt;
  int nerrs = 0;
  for (int c; (c = getopt(argc, (char*const*)argv, "o:ARDSKMh")) != -1; ) switch (c) {
    case 'o': opt_outfile = optarg; break;
    case 'A': opt_printast = true; break;
    case 'R': opt_printir = true; break;
    case 'D': opt_genirdot = true; break;
    case 'S': opt_genasm = true; break;
    case 'K': opt_logld = true; break;
    case 'M': opt_nomain = true; break;
    case 'h': usage(argv[0]); exit(0);
    case ':': warnx("missing value for -%c", optopt); nerrs++; break;
    case '?': warnx("unrecognized option -%c", optopt); nerrs++; break;
  }
  return nerrs > 0 ? -1 : optind;
}


int build_main(int argc, const char** argv) {
  char* coroot = LLVMGetMainExecutable(argv[0]);
  coroot[path_dirlen(coroot, strlen(coroot))] = 0;
  COROOT = coroot;

  int numflags = parse_cli_options(argc, argv);
  if (numflags < 0)
    return 1;

  if (numflags == argc)
    errx(1, "missing input source");

  argv += numflags;
  argc -= numflags;

  sym_init(memalloc_ctx());

  err_t err = build_exe(argv, (usize)argc);
  if (err && err != ErrCanceled)
    dlog("failed to build: %s", err_str(err));

  return err == 0 ? 0 : 1;
}
