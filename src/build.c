#include "c0lib.h"
#include "path.h"
#include "compiler.h"

#include <stdlib.h> // exit
#include <unistd.h> // getopt
#include <string.h> // strdup
#include <err.h>

#include "llvm/llvm.h"


const char* C0ROOT = ""; // Directory of c0 itself; dirname(argv[0])

extern CoLLVMOS host_os; // defined in main.c

// cli options
static const char* opt_outfile = "a.out";


static void diaghandler(const diag_t* d, void* nullable userdata) {
  log("%s", d->msg);
  if (*d->srclines)
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

  compiler_t c;
  compiler_init(&c, memalloc_ctx(), &diaghandler);
  c.triple = llvm_host_triple();
  //c.triple = "aarch64-linux-unknown";
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
  CoLLVMLink link = {
    .target_triple = c.triple,
    .outfile = opt_outfile,
    .infilec = filecount,
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
    "usage: c0 %s [options] <source> ...\n"
    "options:\n"
    "  -o <file>  Write executable to <file> instead of %s\n"
    "  -h         Print help on stdout and exit\n"
    "",
    prog,
    opt_outfile);
}


static int parse_cli_options(int argc, const char** argv) {
  extern char* optarg; // global state in libc... coolcoolcool
  extern int optind, optopt;
  int nerrs = 0;
  for (int c; (c = getopt(argc, (char*const*)argv, "o:h")) != -1; ) switch (c) {
    case 'o': opt_outfile = optarg; break;
    case 'h': usage(argv[0]); exit(0);
    case ':': warnx("missing value for -%c", optopt); nerrs++; break;
    case '?': warnx("unrecognized option -%c", optopt); nerrs++; break;
  }
  return nerrs > 0 ? -1 : optind;
}


int build_main(int argc, const char** argv) {
  // C0ROOT = dirname($0)
  char* C0ROOT = LLVMGetMainExecutable(argv[0]);
  C0ROOT[path_dirlen(C0ROOT, strlen(C0ROOT))] = 0;

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
    log("failed to build: %s", err_str(err));

  return err == 0 ? 0 : 1;
}
