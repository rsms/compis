#include "c0lib.h"
#include "abuf.h"
#include "path.h"
#include "sha256.h"
#include "compiler.h"

#include <stdlib.h> // exit
#include <unistd.h> // getopt
#include <string.h> // strdup
#include <err.h>

#include "llvm/llvm.h"


//          USE                        DEFAULT
// C0ROOT   Directory of co itself     dirname(argv[0])
// C0CACHE  Directory for build cache  .c0cache
const char* C0ROOT = "";
const char* C0CACHE = ".c0cache";

extern CoLLVMOS host_os; // defined in main.c

extern int clang_main(int argc, const char** argv); // llvm/driver.cc
extern int clang_compile(int argc, const char** argv);


static err_t fmt_ofile(input_t* input, buf_t* ofile) {
  const char* objdir = ".c0";
  u8 sha256[32];
  usize needlen = strlen(objdir) + 1 + sizeof(sha256)*2 + 2; // objdir/sha256.o
  for (;;) {
    ofile->len = 0;
    if (!buf_reserve(ofile, needlen))
      return ErrNoMem;
    abuf_t s = abuf_make(ofile->p, ofile->cap);
    abuf_str(&s, objdir);
    abuf_c(&s, PATH_SEPARATOR);
    #if 1
      // compute SHA-256 checksum of input file
      sha256_data(sha256, input->data.p, input->data.size);
      abuf_reprhex(&s, sha256, sizeof(sha256), /*spaced*/false);
    #else
      abuf_str(&s, input->name);
    #endif
    abuf_str(&s, ".o");
    usize n = abuf_terminate(&s);
    if (n < needlen) {
      ofile->len = n + 1;
      return 0;
    }
    needlen = n+1;
  }
}

// ".c0/d957c9d36b0e5dc07b9284b9245a22c0d81cbbbeb2d037f638095adedf20174b.o"


static err_t compile_c_to_o(compiler_t* c, const char* cfile, const char* ofile) {
  dlog("cc %s -> %s", cfile, ofile);
  const char* argv[] = {
    "c0", "-target", c->triple,
    "-std=c17",
    "-O2",
    "-g", "-feliminate-unused-debug-types",
    "-Wall",
    "-Wcovered-switch-default",
    "-Werror=implicit-function-declaration",
    "-Werror=incompatible-pointer-types",
    "-Werror=format-insufficient-args",
    "-c", cfile, "-o", ofile,
  };
  int status = clang_compile(countof(argv), argv);
  return status == 0 ? 0 : ErrInvalid;
}


static err_t compile_co_to_c(compiler_t* c, input_t* input, const char* cfile) {
  // format intermediate C filename cfile
  dlog("[compile_co_to_c] cfile: %s", cfile);
  u32 errcount = c->errcount;

  // parse
  dlog("——————————————— parse ———————————————");
  parser_t parser;
  parser_init(&parser, c);
  memalloc_t ast_ma = c->ma;
  node_t* unit = parser_parse(&parser, ast_ma, input);
  dlog("——————————————— end parse ———————————————");

  // format AST
  buf_t buf = buf_make(c->ma);
  err_t err = node_repr(&buf, unit);
  if (!err)
    log("AST:\n%.*s\n", (int)buf.len, buf.chars);

  node_free(ast_ma, unit);
  parser_dispose(&parser);

  if (err)
    return err;

  if (c->errcount > errcount)
    return ErrCanceled;

  return ErrCanceled; // XXX
  // // pretend we parsed the file and generated C code
  // dlog("genc %s -> %s", input->name, cfile);
  // fs_mkdirs(cfile, path_dirlen(cfile, strlen(cfile)), 0770);
  // return writefile(cfile, 0660, input->data.p, input->data.size);
}


static err_t compile_source_file(compiler_t* c, const char* infile, buf_t* ofile) {
  err_t err;
  dlog("compile_source_file(%s)", infile);

  if (*infile == 0)
    return ErrInvalid;

  input_t* input = input_create(c->ma, infile);
  if (input == NULL)
    return ErrNoMem;

  if ((err = input_open(input)))
    goto end;

  // format output filename ofile
  if UNLIKELY(err = fmt_ofile(input, ofile))
    goto end;
  dlog("ofile: %s", ofile->chars);

  // C source file path
  buf_t cfile = buf_make(c->ma);

  // find input extension
  const char* input_ext = "";
  isize dotpos = slastindexof(input->name, '.');
  if (dotpos > -1)
    input_ext = &input->name[dotpos + 1];

  // do different things depending on the filename extension of strfile
  if (strcmp(input_ext, "c") == 0) {
    // C (cfile == input->name)
    if (!buf_append(&cfile, input->name, strlen(input->name) + 1))
      err = ErrNoMem;
  } else if (strcmp(input_ext, "co") == 0) {
    // co (co => cfile)
    if (!buf_append(&cfile, ofile->p, ofile->len)) {
      err = ErrNoMem;
    } else {
      cfile.chars[cfile.len - 2] = 'c'; // /foo/bar.o\0 -> /foo/bar.c\0
      err = compile_co_to_c(c, input, cfile.chars);
    }
  } else {
    log("%s: unrecognized file type", input->name);
    err = ErrNotSupported;
  }

  // compile C -> object
  if (!err)
    err = compile_c_to_o(c, cfile.chars, ofile->chars);

end:
  input_free(input, c->ma);
  return err;
}


static void diaghandler(const diag_t* d, void* nullable userdata) {
  log("%s", d->msg);
  if (*d->srclines)
    log("%s", d->srclines);
}


static err_t build_exe(const char** srcfilev, usize filecount) {
  err_t err = 0;

  if (filecount == 0)
    return ErrInvalid;

  compiler_t c;
  compiler_init(&c, memalloc_ctx(), &diaghandler);
  c.triple = llvm_host_triple();
  //c.triple = "aarch64-linux-unknown";
  dlog("compiler.triple: %s", c.triple);

  // allocate ofiles
  mem_t ofilesmem = {0};
  array_t ofilesarray = array_make(c.ma);
  buf_t* ofilev = array_alloc(buf_t, &ofilesarray, filecount);
  if (ofilev == NULL) {
    err = ErrNoMem;
    goto end;
  }
  ofilesmem = mem_alloc(c.ma, filecount * sizeof(void*));
  if (ofilesmem.p == NULL) {
    err = ErrNoMem;
    goto end;
  }
  const char** ofiles = ofilesmem.p;

  // compile object files
  for (usize i = 0; i < filecount; i++) {
    buf_init(&ofilev[i], c.ma);
    if (( err = compile_source_file(&c, srcfilev[i], &ofilev[i]) ))
      goto end;
    ofiles[i] = ofilev[i].chars;
  }

  // link executable
  CoLLVMLink link = {
    .target_triple = c.triple,
    .outfile = "out/hello",
    .infilec = filecount,
    .infilev = ofiles,
  };
  err = llvm_link(&link);

end:
  if (ofilesmem.p != NULL)
    mem_free(c.ma, &ofilesmem);
  array_dispose(buf_t, &ofilesarray);
  compiler_dispose(&c);
  return err;
}


static void usage(const char* prog) {
  printf(
    "usage: c0 %s [options] <source> ...\n"
    "options:\n"
    "  -h  Print help on stdout and exit\n"
    "",
    prog);
}


static int parse_cli_options(int argc, const char** argv) {
  extern char* optarg; // global state in libc... coolcoolcool
  extern int optind, optopt;
  int nerrs = 0;
  for (int c; (c = getopt(argc, (char*const*)argv, ":hp")) != -1; ) switch (c) {
    case 'h': usage(argv[0]); exit(0);
    case ':': warnx("option -%c requires a value", optopt); nerrs++; break;
    case '?': warnx("unrecognized option -%c", optopt); nerrs++; break;
  }
  return nerrs > 0 ? -1 : optind;
}


int build_main(int argc, const char** argv) {
  // C0ROOT = dirname($0)
  char* C0ROOT = LLVMGetMainExecutable(argv[0]);
  C0ROOT[path_dirlen(C0ROOT, strlen(C0ROOT))] = 0;

  int argi = parse_cli_options(argc, argv);
  if (argi < 0)
    return 1;

  if (argi == argc)
    errx(1, "missing input source");

  int numflags = argc - argi;
  argv += numflags;
  argc -= numflags;

  err_t err = build_exe(argv, (usize)argc);
  if (err)
    log("failed to build: %s", err_str(err));

  return err == 0 ? 0 : 1;
}
