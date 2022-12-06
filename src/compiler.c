// SPDX-License-Identifier: Apache-2.0
#include "c0lib.h"
#include "compiler.h"
#include "path.h"
#include "abuf.h"
#include "sha256.h"
#include "llvm/llvm.h"

#include <unistd.h> // fork
#include <err.h>

// build.c
extern const char* C0ROOT;


static void set_cachedir(compiler_t* c, slice_t cachedir) {
  c->cachedir = mem_strdup(c->ma, cachedir, 0);
  safecheck(c->cachedir);
  c->objdir = mem_strcat(c->ma, cachedir, slice_cstr(PATH_SEPARATOR_STR "obj"));
  safecheck(c->objdir);
}


static void set_cflags(compiler_t* c) {
  buf_t buf = buf_make(c->ma);
  buf_printf(&buf, "-I%s/../../lib", C0ROOT);
  bool ok = buf_nullterm(&buf);
  safecheck(ok);
  c->cflags = buf.chars;
}


void compiler_set_triple(compiler_t* c, const char* triple) {
  c->triple = triple;
  CoLLVMTargetInfo info;
  llvm_triple_info(triple, &info);
  c->ptrsize = info.ptr_size;
  c->isbigendian = !info.is_little_endian;
}


void compiler_init(compiler_t* c, memalloc_t ma, diaghandler_t dh) {
  memset(c, 0, sizeof(*c));
  c->ma = ma;
  c->diaghandler = dh;
  buf_init(&c->diagbuf, c->ma);
  compiler_set_triple(c, llvm_host_triple());
  set_cachedir(c, slice_cstr(".c0"));
  set_cflags(c);
  if (!map_init(&c->typeidmap, c->ma, 16))
    panic("out of memory");
}


void compiler_dispose(compiler_t* c) {
  buf_dispose(&c->diagbuf);
  map_dispose(&c->typeidmap, c->ma);
}


void compiler_set_cachedir(compiler_t* c, slice_t cachedir) {
  mem_freex(c->ma, MEM(c->cachedir, strlen(c->cachedir) + 1));
  mem_freex(c->ma, MEM(c->objdir, strlen(c->objdir) + 1));
  set_cachedir(c, cachedir);
}


//————————————————————————————————————————————————————————————————————————————
// compiler_compile


// llvm/clang_compile.cc
extern int clang_compile(int argc, const char** argv);


static err_t compile_c_async(
  compiler_t* c, promise_t* p, const char* cfile, const char* ofile)
{
  // clang crashes if we run it more than once in the same process, so we fork
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
    c->cflags,
    "-c", "-xc", cfile,
    "-o", ofile,
  };

  pid_t pid = fork();
  if (pid == -1) {
    warn("fork");
    return ErrCanceled;
  }

  if (pid == 0) {
    int status = clang_compile(countof(argv), argv);
    _exit(status);
    UNREACHABLE;
  } else {
    promise_open(p, pid);
    return 0;
  }
}


static err_t dump_ast(const node_t* ast) {
  buf_t buf = buf_make(memalloc_ctx());
  err_t err = node_repr(&buf, ast);
  if (!err) {
    fwrite(buf.chars, buf.len, 1, stderr);
    fputc('\n', stderr);
  }
  buf_dispose(&buf);
  return err;
}


static err_t compile_co_to_c(compiler_t* c, input_t* input, const char* cfile) {
  // format intermediate C filename cfile
  dlog("[compile_co_to_c] cfile: %s", cfile);
  u32 errcount = c->errcount;
  err_t err = 0;

  // bump allocator for AST
  mem_t ast_mem = mem_alloc_zeroed(c->ma, 1024*1024*100);
  if (ast_mem.p == NULL)
    return ErrNoMem;
  memalloc_t ast_ma = memalloc_bump(ast_mem.p, ast_mem.size, MEMALLOC_STORAGE_ZEROED);

  // parse
  dlog("————————— parse —————————");
  parser_t parser;
  if (!parser_init(&parser, c)) {
    err = ErrNoMem;
    goto end;
  }
  unit_t* unit = parser_parse(&parser, ast_ma, input);
  dlog("————————— AST —————————");
  if ((err = dump_ast((node_t*)unit)))
    goto end_parser;

  // bail on parse error
  if (c->errcount > errcount) {
    err = ErrCanceled;
    goto end_parser;
  }

  // analyze
  dlog("————————— analyze —————————");
  if (( err = analyze(&parser, unit) ))
    goto end_parser;
  dlog("————————— AST —————————");
  if ((err = dump_ast((node_t*)unit)))
    goto end_parser;
  if (c->errcount > errcount) {
    err = ErrCanceled;
    goto end_parser;
  }

  // analyze2 (ir)
  dlog("————————— analyze2 —————————");
  memalloc_t ir_ma = ast_ma;
  if (( err = analyze2(c, ir_ma, unit) ))
    goto end_parser;

  // generate C code
  dlog("————————— cgen —————————");
  cgen_t g;
  if (!cgen_init(&g, c, c->ma))
    goto end_parser;
  err = cgen_generate(&g, unit);
  if (!err) {
    #if DEBUG
      fprintf(stderr, "——————————\n%.*s\n——————————\n",
        (int)g.outbuf.len, g.outbuf.chars);
    #endif
    dlog("cgen %s -> %s", input->name, cfile);
    err = writefile(cfile, 0660, buf_slice(g.outbuf));
  }
  cgen_dispose(&g);

end_parser:
  parser_dispose(&parser);
end:
  mem_free(c->ma, &ast_mem);
  if (c->errcount > errcount && !err)
    err = ErrCanceled;
  return err;
}


static err_t fmt_ofile(compiler_t* c, input_t* input, buf_t* ofile) {
  u8 sha256[32];
  usize needlen = strlen(c->objdir) + 1 + sizeof(sha256)*2 + 2; // objdir/sha256.o
  for (;;) {
    ofile->len = 0;
    if (!buf_reserve(ofile, needlen))
      return ErrNoMem;
    abuf_t s = abuf_make(ofile->p, ofile->cap);
    abuf_str(&s, c->objdir);
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


err_t compiler_compile(compiler_t* c, promise_t* p, input_t* input, buf_t* ofile) {
  err_t err = fmt_ofile(c, input, ofile);
  if (err)
    return err;

  // C source file path
  buf_t cfile = buf_make(c->ma);

  // do different things depending on the input type
  switch (input->type) {

  case FILE_C: // C (cfile == input->name)
    if (!buf_append(&cfile, input->name, strlen(input->name) + 1))
      err = ErrNoMem;
    break;

  case FILE_CO: // co (co => cfile)
    if (!buf_append(&cfile, ofile->p, ofile->len)) {
      err = ErrNoMem;
    } else {
      cfile.chars[cfile.len - 2] = 'c'; // /foo/bar.o\0 -> /foo/bar.c\0
      err = compile_co_to_c(c, input, cfile.chars);
    }
    break;

  // case FILE_O:
  //   // TODO: hard link or copy input to ofile
  //   break;

  default:
    log("%s: unrecognized file type", input->name);
    err = ErrNotSupported;
  }

  // compile C -> object
  if (!err)
    err = compile_c_async(c, p, cfile.chars, ofile->chars);

  buf_dispose(&cfile);
  return err;
}
