#include "colib.h"
#include "compiler.h"
#include "dirwalk.h"
#include "path.h"
#include "s-expr.h"

#include <stdlib.h>
#include <err.h>


typedef array_type(diag_t) diagarray_t;
DEF_ARRAY_TYPE_API(diag_t, diagarray)


// cli options
static bool opt_help = false;
static bool opt_v = false; // ignored; we use coverbose instead
static bool opt_colors = false;
static bool opt_no_colors = false;

#define FOREACH_CLI_OPTION(S, SV, L, LV,  DEBUG_L, DEBUG_LV) \
  /* S( var, ch, name,          descr) */\
  /* SV(var, ch, name, valname, descr) */\
  /* L( var,     name,          descr) */\
  /* LV(var,     name, valname, descr) */\
  L( &opt_colors,    "colors",    "Enable colors regardless of TTY status")\
  L( &opt_no_colors, "no-colors", "Disable colors regardless of TTY status")\
  S( &opt_v,    'v', "verbose",   "Verbose mode")\
  S( &opt_help, 'h', "help",      "Print help on stdout and exit")\
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
  err_t err = ast_repr(&buf, ast, /*flags*/AST_REPR_TYPES);
  if (!err) {
    fwrite(buf.chars, buf.len, 1, stderr);
    fputc('\n', stderr);
  }
  buf_dispose(&buf);
  return err;
}


// static bool is_filename_compis_source(const char* name) {
//   usize namelen = strlen(name);

//   isize p = string_lastindexof(name, namelen, '.');
//   if (p <= 0 || (usize)p + 1 == namelen)
//     return false;

//   const char* ext = &name[p+1];
//   if (!strieq(ext, "co") && !strieq(ext, "c"))
//     return false;

//   return true;
// }


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


static void elog_diag(const diag_t* d) {
  elog("%s", d->msg);
  if (d->srclines && *d->srclines)
    elog("%s", d->srclines);
}


static void parser_test_diaghandler(const diag_t* d, void* nullable userdata) {
  diagarray_t* diags = userdata;
  if (diags) {
    diag_t* d2 = diagarray_alloc(diags, memalloc_ctx(), 1);
    if UNLIKELY(!d2 || !diag_copy(d2, d, memalloc_ctx()))
      errx(1, "out of memory");
  }
  if (!diags || coverbose > 1)
    elog_diag(d);
}


static bool diff_s_expr(slice_t actual, slice_t expect, const char* filename) {
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
    elog("%s: expected AST:\n"
         "————————————————————————————————————————————————————————————\n"
         "%.*s",
         relpath(filename), (int)expect.len, expect.chars);
    elog("———————————————————————— actual AST ————————————————————————\n"
         "%.*s\n"
         "————————————————————————————————————————————————————————————",
         (int)actual.len, actual.chars);
  }

  buf_dispose(&buf);
  return ok;
}


static slice_t comment_directive_slice(const comment_t* comment, usize prefixlen) {
  slice_t result;
  result.bytes = comment->bytes + prefixlen;
  result.len = comment->len - prefixlen;
  if (string_endswithn((char*)result.bytes, result.len, "*/", 2))
    result.len -= 2;
  return slice_trim(result);
}


static u32 find_directive_comment(
  const parser_t* parser, const char* directive, slice_t* resultv, u32 resultcap)
{
  if (resultcap == 0)
    return 0;
  u32 count = 0;
  for (u32 i = 0; i < parser->scanner.comments.len; i++) {
    comment_t* comment = &parser->scanner.comments.v[i];
    if (comment->len < 4 || comment->bytes[2] != '!')
      continue;
    const char* c_str = (char*)comment->bytes + 3;
    usize c_len = comment->len - 3;
    usize directive_len = strlen(directive);
    if (string_startswithn(c_str, c_len, directive, directive_len)) {
      resultv[count++] = comment_directive_slice(comment, directive_len + 3);
      if (count == resultcap)
        break;
    }
  }
  return count;
}


static unit_t* nullable parse_single_file(
  parser_t* parser, str_t filename, usize filesize)
{
  err_t e;

  pkg_t* pkg = create_pkg("main");
  pkg->defs.parent = &parser->scanner.compiler->builtins;

  // reset diag array
  diagarray_t* diags = parser->scanner.compiler->userdata;
  for (u32 i = 0; i < diags->len; i++)
    diag_free_copy(&diags->v[i], memalloc_ctx());
  diags->len = 0;

  srcfile_t* srcfile = pkg_add_srcfile(pkg, filename.p, filename.len, NULL);
  if (!srcfile) errx(1, "pkg_add_srcfile failed");
  srcfile->size = filesize;
  if (( e = srcfile_open(srcfile) ))
    errx(1, "srcfile_open(%s): %s", filename.p, err_str(e));

  unit_t* unit = NULL;
  memalloc_t ast_ma = memalloc_ctx();
  if (( e = parser_parse(parser, ast_ma, srcfile, &unit) ))
    errx(1, "parser_parse: %s", err_str(e));

  if (coverbose) {
    log("————————— AST %s (parse) —————————", relpath(filename.p));
    dump_ast((node_t*)unit);
  }

  return unit;
}


static bool check_expected_ast(
  parser_t* parser, str_t filename, unit_t* unit, slice_t expect_ast)
{
  if (coverbose > 1) {
    log("———————— verbatim expectation ————————\n"
        "%.*s\n"
        "——————————————————————————————————————",
        (int)expect_ast.len, expect_ast.chars);
  }

  buf_t buf = buf_make(memalloc_ctx());
  u32 flags = AST_REPR_SIMPLE_UNIT;
  err_t e = ast_repr(&buf, (node_t*)unit, flags);
  assertf(e == 0, "ast_repr: %s", err_str(e));
  slice_t actual_ast = buf_slice(buf);

  bool pass = diff_s_expr(actual_ast, expect_ast, filename.p);
  buf_dispose(&buf);
  return pass;
}


static bool find_matching_diag_msg(diagarray_t* diags, u32* diag_i, slice_t subject) {
  while (*diag_i < diags->len) {
    const char* diag_msg = diags->v[(*diag_i)++].msg;
    usize diag_msg_len = strlen(diag_msg);
    // if (string_endswithn(diag_msg, diag_msg_len, subject.chars, subject.len))
    //   return true;
    if (string_indexofstr(diag_msg, diag_msg_len, subject.chars, subject.len) > -1)
      return true;
  }
  return false;
}


static bool check_expected_diag(
  parser_t* parser, str_t filename, diagarray_t* diags, slice_t expect)
{
  // format of '!expect-diag' is one diag.msg suffix per line.
  // For example:
  //   /*!expect-diag
  //     error: redefinition of "a"
  //     previously defined here
  //   */
  // matches actual output:
  //   filename.co:9:1: error: redefinition of "a"
  //    8   │ let a = 1
  //    9 → │ var a = 2
  //   10   │
  //   filename.co:8:1: help: "a" previously defined here
  //   8 → │ let a = 1
  //

  // Process each line
  // Note: expect already has whitespace trimmed around it.
  u32 diag_i = 0;
  for (slice_t expect_line = {}; slice_iterlines(expect, &expect_line);) {
    expect_line = slice_trim(expect_line);

    // skip empty lines
    if (expect_line.len == 0)
      continue;

    // check for match
    if LIKELY(find_matching_diag_msg(diags, &diag_i, expect_line)) {
      vvlog("%s: found expected diagnostic \"%.*s\"",
            relpath(filename.p), (int)expect_line.len, expect_line.chars);
    } else {
      // check if the order is different, rather than the expected message missing
      diag_i = 0;
      if (find_matching_diag_msg(diags, &diag_i, expect_line)) {
        elog("Expected diagnostic missing or out of order:");
        elog("  %.*s", (int)expect_line.len, expect_line.chars);
      } else {
        elog("Expected diagnostic not found:");
        elog("  %.*s", (int)expect_line.len, expect_line.chars);
      }
      elog("Actual diagnostics:");
      for (u32 i = 0; i < diags->len; i++)
        elog("  %s", diags->v[i].msg);
      return false;
    }
  }

  return true;
}


static bool parser_test_one(parser_t* parser, str_t filename, usize filesize) {
  bool pass = true;

  compiler_t* compiler = parser->scanner.compiler;
  compiler_errcount_reset(compiler);

  u64 timespent = nanotime();

  unit_t* unit = parse_single_file(parser, filename, filesize);
  diagarray_t* diags = parser->scanner.compiler->userdata;

  timespent = nanotime() - timespent;

  slice_t expect_diagv[32];
  u32 expect_diagc = find_directive_comment(
    parser, "expect-diag", expect_diagv, countof(expect_diagv));

  slice_t expect_astv[2];
  u32 expect_astc = find_directive_comment(
    parser, "expect-ast", expect_astv, countof(expect_astv));

  // expect-diag
  if (expect_diagc > 0) {
    // typecheck & analyze if there's no AST check and no parse errors occurred
    if (expect_astc == 0 && compiler_errcount(compiler) == 0) {
      memalloc_t ast_ma = parser->scanner.ast_ma;
      pkg_t* pkg = assertnotnull(parser->scanner.srcfile->pkg);
      err_t err = typecheck(compiler, ast_ma, pkg, &unit, 1);
      if (err) {
        elog("typecheck failed: %s", err_str(err));
        pass = false;
        goto end;
      }
      if (coverbose) {
        log("————————— AST %s (typecheck) —————————", relpath(filename.p));
        dump_ast((node_t*)unit);
      }
      if (compiler_errcount(compiler) == 0) {
        if (( err = iranalyze(compiler, ast_ma, pkg, &unit, 1) )) {
          dlog("iranalyze failed: %s", err_str(err));
          pass = false;
          goto end;
        }
      }
    }

    bool expect_error = false;
    for (u32 i = 0; i < expect_diagc; i++) {
      if (!check_expected_diag(parser, filename, diags, expect_diagv[i]))
        pass = false;
      if ( !expect_error &&
           string_indexofstr(
             expect_diagv[i].chars, expect_diagv[i].len,
             "error:", strlen("error:")) > -1 )
      {
        expect_error = true;
      }
    }
    // make sure parsing failed if there was an error,
    // or that parsing succeeded if no error was expected
    if (pass && expect_error != compiler_errcount(compiler) > 0) {
      if (expect_error) {
        elog("%s: parsing succeeded even though an error was expected",
             relpath(filename.p));
      } else {
        elog("%s: parsing failed even though no error was expected",
             relpath(filename.p));
        elog("Tip: Add a comment like this to signal that an error is expected:");
        elog("  //!expect-diag error:");
      }
      pass = false;
    }
  }
  if (expect_diagc == 0 || (!pass && coverbose > 1)) {
    for (u32 i = 0; i < diags->len; i++)
      elog_diag(&diags->v[i]);
    if (compiler_errcount(compiler) > 0)
      pass = false;
  }

  // expect-ast
  if (expect_astc) {
    if (!unit) {
      vlog("%s: no parser output (no unit)", filename.p);
      pass = false;
    } else if (compiler_errcount(compiler) > 0) {
      vlog("%s: syntax errors", filename.p);
      pass = false;
    } else if (!check_expected_ast(parser, filename, unit, expect_astv[0])) {
      pass = false;
    }
    if (expect_astc > 1) {
      elog("%s: more than one !expect-ast directive (not supported)",
           relpath(filename.p));
      pass = false;
    }
  }

end:
  char duration[25];
  fmtduration(duration, timespent);
  log("%s: %s (%s)",
      relpath(filename.p),
      opt_colors ?
        (pass ? "\e[1;32mPASS\e[0m" : "\e[1;37;41m FAIL \e[0m") :
        (pass ? "PASS" : "FAIL"),
      duration);

  return pass;
}


typedef struct fileinfo_t {
  char* name;
  usize size;
} fileinfo_t;


static int fileinfo_cmp(const void* x, const void* y, void* nullable ctx) {
  const fileinfo_t* a = x;
  const fileinfo_t* b = y;
  return strcasecmp(a->name, b->name);
}


static usize find_co_files(str_t dirpath, fileinfo_t* filev, usize filecap) {
  err_t e;
  memalloc_t ma = memalloc_ctx();
  usize i = 0;
  dirwalk_t* dw;

  if UNLIKELY(( e = dirwalk_open(&dw, ma, dirpath.p, 0) ))
    errx(1, "dirwalk_open %s: %s", dirpath.p, err_str(e));

  while ((e = dirwalk_next(dw)) > 0) {
    if (dw->type != S_IFREG || filetype_guess(dw->name) != FILE_CO)
      continue;
    if (i == filecap)
      errx(1, "too many files in %s", dirpath.p);
    filev[i].name = mem_strdup(ma, slice_cstr(dw->name), 0);
    filev[i].size = dirwalk_stat(dw)->st_size;
    i++;
  }

  if (e < 0) errx(1, "dirwalk(%s): %s", dirpath.p, err_str(e));
  dirwalk_close(dw);

  co_qsort(filev, i, sizeof(*filev), fileinfo_cmp, NULL);

  return i;
}


static bool parser_tests(memalloc_t ma) {
  // create compiler instance
  compiler_t compiler;
  create_compiler(&compiler, parser_test_diaghandler, &(compiler_config_t){
    // we won't be loading std/runtime
    .nostdruntime = true,
  });

  // create buffer for capturing diagnostics
  compiler.userdata = assertnotnull(mem_alloct(ma, diagarray_t));

  // create parser instance
  parser_t parser;
  if (!parser_init(&parser, &compiler))
    errx(1, "parser_init: %s", err_str(ErrNoMem));
  parser.scanner.parse_comments = true;

  bool pass = true;

  // parse all files in the "syntax" directory
  fileinfo_t filev[128];
  str_t dirpath = path_join(coroot, "..", "test", "syntax");
  usize filenamec = find_co_files(dirpath, filev, countof(filev));
  str_t filename = {};
  for (usize i = 0; i < filenamec; i++) {
    str_free(filename);
    filename = path_join(dirpath.p, filev[i].name);
    if (!parser_test_one(&parser, filename, filev[i].size))
      pass = false;
  }

  parser_dispose(&parser);
  compiler_dispose(&compiler);

  return pass;
}


int main_selftest(int argc, char* argv[]) {
  if (!cliopt_parse(&argc, &argv, help))
    return 1;

  // if neither of --color or --no-color is specified, enable if stderr is a TTY
  if (!opt_colors && !opt_no_colors && isatty(2))
    opt_colors = true;

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
  if (!parser_tests(ma))
    exit(1);

  // restore memory allocator and return
  memalloc_ctx_set(ma_outer);
  memalloc_bump2_dispose(ma);
  return 0;
}
