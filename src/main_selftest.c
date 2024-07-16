#include "colib.h"
#include "compiler.h"
#include "dirwalk.h"
#include "path.h"
#include "s-expr.h"

#include <stdlib.h>
#include <err.h>


// cli options
static bool opt_help = false;
static bool opt_v = false; // ignored; we use coverbose instead

#define FOREACH_CLI_OPTION(S, SV, L, LV,  DEBUG_L, DEBUG_LV) \
  /* S( var, ch, name,          descr) */\
  /* SV(var, ch, name, valname, descr) */\
  /* L( var,     name,          descr) */\
  /* LV(var,     name, valname, descr) */\
  S( &opt_v,    'v', "verbose", "Verbose mode")\
  S( &opt_help, 'h', "help",    "Print help on stdout and exit")\
// end FOREACH_CLI_OPTION

#include "cliopt.inc.h"

static void help(const char* cmdname) {
  printf(
    "Run Compis tests\n"
    "Usage: %s %s [options]\n"
    "Options:\n"
    "",
    coprogname, cmdname);
  cliopt_print();
  exit(0);
}


u32 unittest_runall(); // unittest.c


static err_t dump_ast(const node_t* ast) {
  buf_t buf = buf_make(memalloc_ctx());
  err_t err = ast_repr(&buf, ast, /*flags*/0);
  if (!err) {
    fwrite(buf.chars, buf.len, 1, stderr);
    fputc('\n', stderr);
  }
  buf_dispose(&buf);
  return err;
}


static bool is_filename_compis_source(const char* name) {
  usize namelen = strlen(name);

  isize p = string_lastindexof(name, namelen, '.');
  if (p <= 0 || (usize)p + 1 == namelen)
    return false;

  const char* ext = &name[p+1];
  if (!strieq(ext, "co") && !strieq(ext, "c"))
    return false;

  return true;
}


static void create_compiler(
  compiler_t* c, diaghandler_t dh, const compiler_config_t* cfgp)
{
  compiler_init(c, memalloc_ctx(), dh);
  compiler_config_t cfg = *cfgp;

  if (!cfg.target)
    cfg.target = target_default();

  if (!cfg.buildroot) {
    const char* builddir = "/tmp/compis-selftest-build"; // TODO FIXME
    fs_remove(builddir);
    cfg.buildroot = builddir;
  }

  cfg.verbose = coverbose;

  err_t e = compiler_configure(c, &cfg);
  if (e) err(1, "compiler_configure: %s", err_str(e));
}


static pkg_t* create_pkg(const char* pkgpath) {
  pkg_t* pkg = mem_alloct(memalloc_ctx(), pkg_t);
  if (!pkg) errx(1, "out of memory");
  err_t e = pkg_init(pkg, memalloc_ctx());
  if (e) errx(1, "pkg_init: %s", err_str(e));
  pkg->path = str_make(pkgpath);
  pkg->root = str_make("");
  pkg->dir = str_make("");
  pkg->isadhoc = true;
  return pkg;
}


// static srcfile_t* add_srcfile_buf(
//   pkg_t* pkg, const char* filename, const char* source)
// {
//   srcfile_t* srcfile = pkg_add_srcfile(pkg, filename, strlen(filename), NULL);
//   if (!srcfile) errx(1, "pkg_add_srcfile failed");
//   srcfile->data = source;
//   srcfile->size = strlen(source);
//   return srcfile;
// }


/*static unit_t* parse_one_buf(
  parser_t* parser, const char* filename, const char* source)
{
  // create package
  pkg_t* pkg = create_pkg("main");

  // add source file to package
  srcfile_t* srcfile = add_srcfile_buf(pkg, filename, source);

  // parse source file
  unit_t* unit;
  memalloc_t ast_ma = memalloc_ctx();
  err_t e = parser_parse(parser, ast_ma, srcfile, &unit);
  if (e) errx(1, "parser_parse: %s", err_str(e));

  if (coverbose) {
    log("————————— AST %s —————————", filename);
    dump_ast((node_t*)unit);
  }

  return unit;
}*/


static bool diff_s_expr(slice_t actual, slice_t expect) {
  err_t e;
  buf_t buf = buf_make(memalloc_ctx());
  buf_reserve(&buf, actual.len*3);

  e = s_expr_prettyprint(&buf, actual);
  assertf(e == 0, "s_expr_prettyprint: %s", err_str(e));

  usize actual_buf_len = buf.len;

  e = s_expr_prettyprint(&buf, expect);
  assertf(e == 0, "s_expr_prettyprint: %s", err_str(e));

  actual = buf_slice(buf, 0, actual_buf_len);
  expect = buf_slice(buf, actual_buf_len, buf.len - actual_buf_len);

  bool ok = slice_eq(actual, expect);

  if (!ok) {
    // FIXME: better display of mismatch
    elog("———— expected ————\n%.*s", (int)expect.len, expect.chars);
    elog("———— actual ————\n%.*s", (int)actual.len, actual.chars);
  }

  buf_dispose(&buf);
  return ok;
}


static unit_t* parse_one_file(parser_t* parser, str_t filename, usize filesize) {
  err_t e;

  pkg_t* pkg = create_pkg("main");

  srcfile_t* srcfile = pkg_add_srcfile(pkg, filename.p, filename.len, NULL);
  if (!srcfile) errx(1, "pkg_add_srcfile failed");
  srcfile->size = filesize;
  if (( e = srcfile_open(srcfile) ))
    errx(1, "srcfile_open(%s): %s", filename.p, err_str(e));

  unit_t* unit;
  memalloc_t ast_ma = memalloc_ctx();
  if (( e = parser_parse(parser, ast_ma, srcfile, &unit) ))
    errx(1, "parser_parse: %s", err_str(e));

  if (coverbose) {
    log("————————— AST %s —————————", relpath(filename.p));
    dump_ast((node_t*)unit);
  }

  if (parser_errcount(parser) > 0)
    errx(1, "syntax errors; expected parsing to succeed");

  // check AST
  buf_t buf = buf_make(memalloc_ctx());
  e = ast_repr(&buf, (node_t*)unit, /*flags*/0);
  assertf(e == 0, "ast_repr: %s", err_str(e));
  slice_t actual = buf_slice(buf);
  slice_t expect = slice_cstr(
    "(UNIT /Users/rsms/src/compis/test/syntax/tokens.co\n"
    "  (LET _ (INTLIT 1)))\n");
  if (diff_s_expr(actual, expect)) {
    log("%s: OK", relpath(filename.p));
  } else {
    log("%s: FAIL", relpath(filename.p));
  }
  buf_dispose(&buf);

  return unit;
}


static void parser_test_diaghandler(const diag_t* d, void* nullable userdata) {
  elog("[%s] %s", __FUNCTION__, d->msg);
  if (d->srclines && *d->srclines)
    elog("%s", d->srclines);
}


static void parser_tests(memalloc_t ma) {
  err_t e;

  // create compiler instance
  compiler_t compiler;
  create_compiler(&compiler, parser_test_diaghandler, &(compiler_config_t){});

  // create parser instance
  parser_t parser;
  if (!parser_init(&parser, &compiler))
    errx(1, "parser_init: %s", err_str(ErrNoMem));
  parser.scanner.parse_comments = true;

  // parse all files in the "syntax" directory
  dirwalk_t* dw;
  str_t dirpath = path_join(coroot, "..", "test", "syntax");
  str_t filename = {};
  if (( e = dirwalk_open(&dw, ma, dirpath.p, 0) ))
    errx(1, "dirwalk_open: %s", err_str(e));
  while ((e = dirwalk_next(dw)) > 0) {
    if (dw->type != S_IFREG || !is_filename_compis_source(dw->name)) // ignore
      continue;
    str_free(filename);
    filename = path_join(dirpath.p, dw->name);
    parse_one_file(&parser, filename, dirwalk_stat(dw)->st_size);
  }
  if (e < 0) errx(1, "dirwalk(%s): %s", dirpath.p, err_str(e));
  dirwalk_close(dw);

  parser_dispose(&parser);
  compiler_dispose(&compiler);
}


int main_selftest(int argc, char* argv[]) {
  if (!cliopt_parse(&argc, &argv, help))
    return 1;

  // run all integrated unit tests (defined with UNITTEST_DEF)
  if (unittest_runall())
    return 1;

  // create an arena memory allocator that we use for everything
  memalloc_t ma = memalloc_bump2(/*slabsize*/0, /*flags*/0);
  if (ma == memalloc_null())
    errx(1, "memalloc_bump2 failed");
  memalloc_t ma_outer = memalloc_ctx_set(ma);

  // run parser tests
  memalloc_bump2_reset(ma, 0);
  parser_tests(ma);

  // restore memory allocator and return
  memalloc_ctx_set(ma_outer);
  memalloc_bump2_dispose(ma);
  return 0;
}
