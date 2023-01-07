// SPDX-License-Identifier: Apache-2.0
#include "colib.h"
#include "compiler.h"
#include "path.h"
#include "abuf.h"
#include "sha256.h"
#include "subproc.h"
#include "llvm/llvm.h"

#include <err.h>


extern const char* COROOT; // build.c


static void mem_freecstr(memalloc_t ma, char* nullable cstr) {
  if (cstr)
    mem_freex(ma, MEM(cstr, strlen(cstr) + 1));
}


static void set_cstr(memalloc_t ma, char*nullable* dst, slice_t src) {
  mem_freecstr(ma, *dst);
  *dst = mem_strdup(ma, src, 0);
  safecheck(*dst);
}


void compiler_set_pkgname(compiler_t* c, slice_t pkgname) {
  set_cstr(c->ma, &c->pkgname, pkgname);
}


void compiler_init(compiler_t* c, memalloc_t ma, diaghandler_t dh, slice_t pkgname) {
  memset(c, 0, sizeof(*c));
  c->ma = ma;
  c->diaghandler = dh;
  buf_init(&c->diagbuf, c->ma);
  compiler_set_pkgname(c, pkgname);
  if (!map_init(&c->typeidmap, c->ma, 16))
    panic("out of memory");
}


void compiler_dispose(compiler_t* c) {
  buf_dispose(&c->diagbuf);
  map_dispose(&c->typeidmap, c->ma);
  locmap_dispose(&c->locmap, c->ma);
  mem_freecstr(c->ma, c->triple);
  mem_freecstr(c->ma, c->buildroot);
  mem_freecstr(c->ma, c->builddir);
  mem_freecstr(c->ma, c->pkgbuilddir);
  mem_freecstr(c->ma, c->pkgname);
  for (u32 i = 0; i < c->cflags.len; i++)
    mem_freecstr(c->ma, c->cflags.v[i]);
}


static void set_secondary_pointer_types(compiler_t* c) {
  // "&[u8]" -- slice of u8 array
  memset(&c->u8stype, 0, sizeof(c->u8stype));
  c->u8stype.kind = TYPE_SLICE;
  c->u8stype.flags = NF_CHECKED;
  c->u8stype.size = c->ptrsize;
  c->u8stype.align = c->ptrsize;
  c->u8stype.elem = type_u8;

  // "type string &[u8]"
  memset(&c->strtype, 0, sizeof(c->strtype));
  c->strtype.kind = TYPE_ALIAS;
  c->strtype.flags = NF_CHECKED;
  c->strtype.size = c->ptrsize;
  c->strtype.align = c->ptrsize;
  c->strtype.name = sym_str;
  c->strtype.elem = (type_t*)&c->u8stype;
}


static void configure_target(compiler_t* c, const char* triple) {
  set_cstr(c->ma, &c->triple, slice_cstr(triple));
  CoLLVMTargetInfo info;
  llvm_triple_info(triple, &info);
  c->intsize = info.ptr_size;
  c->ptrsize = info.ptr_size;
  c->isbigendian = !info.is_little_endian;
  switch (info.ptr_size) {
    case 1:
      c->addrtype = type_u8;
      c->uinttype = type_u8;
      c->inttype  = type_i8;
      break;
    case 2:
      c->addrtype = type_u16;
      c->uinttype = type_u16;
      c->inttype  = type_i16;
      break;
    case 4:
      c->addrtype = type_u32;
      c->uinttype = type_u32;
      c->inttype  = type_u32;
      break;
    default:
      assert(info.ptr_size <= 8);
      c->addrtype = type_u64;
      c->uinttype = type_u64;
      c->inttype  = type_i64;
  }
  set_secondary_pointer_types(c);
}


static const char* buildmode_name(buildmode_t m) {
  switch ((enum buildmode)m) {
    case BUILDMODE_DEBUG: return "debug";
    case BUILDMODE_OPT:   return "opt";
  }
  return "unknown";
}


static void configure_buildroot(compiler_t* c, slice_t buildroot) {
  // builddirm = {builddir}/{mode}-{triple}
  //           | {builddir}/{mode}  when triple == llvm_host_triple()
  //
  // pkgbuilddir = {builddirm}/{pkgname}.pkg

  set_cstr(c->ma, &c->buildroot, buildroot);

  slice_t mode = slice_cstr(buildmode_name(c->buildmode));
  slice_t triple = slice_cstr(c->triple);

  usize len = buildroot.len + 1 + mode.len;

  bool isnativetarget = strcmp(llvm_host_triple(), c->triple) == 0;
  if (!isnativetarget)
    len += triple.len + 1;

  #define APPEND(slice)  memcpy(p, (slice).p, (slice).len), p += (slice.len)

  mem_freecstr(c->ma, c->builddir);
  c->builddir = mem_alloctv(c->ma, char, len + 1);
  safecheck(c->builddir);
  char* p = c->builddir;
  APPEND(buildroot);
  *p++ = PATH_SEPARATOR;
  APPEND(mode);
  if (!isnativetarget) {
    *p++ = '-';
    APPEND(triple);
  }
  *p = 0;

  // pkgbuilddir
  slice_t pkgname = slice_cstr(c->pkgname);
  slice_t suffix = slice_cstr(".pkg");
  slice_t builddir = { .p = c->builddir, .len = len };
  usize pkgbuilddir_len = len + 1 + pkgname.len + suffix.len;

  mem_freecstr(c->ma, c->pkgbuilddir);
  c->pkgbuilddir = mem_alloctv(c->ma, char, pkgbuilddir_len + 1);
  safecheck(c->pkgbuilddir);
  p = c->pkgbuilddir;
  APPEND(builddir);
  *p++ = PATH_SEPARATOR;
  APPEND(pkgname);
  APPEND(suffix);
  *p = 0;

  #undef APPEND
}


static err_t configure_cflags(compiler_t* c) {
  buf_t* tmpbuf = &c->diagbuf;
  bool ok = true;

  #define APPEND_SLICE(slice) \
    ok &= ptrarray_push(&c->cflags, c->ma, mem_strdup(c->ma, (slice), 0));
  #define APPEND_STR(cstr) \
    APPEND_SLICE(slice_cstr(cstr))

  APPEND_STR("-std=c17");
  APPEND_STR("-g");
  APPEND_STR("-feliminate-unused-debug-types");
  APPEND_STR("-target"); APPEND_STR(c->triple);

  switch ((enum buildmode)c->buildmode) {
    case BUILDMODE_DEBUG:
      APPEND_STR("-O0");
      break;
    case BUILDMODE_OPT:
      APPEND_STR("-O2");
      APPEND_STR("-fomit-frame-pointer");
      break;
  }

  buf_clear(tmpbuf);
  buf_printf(tmpbuf, "-I%s/../../lib", COROOT);
  APPEND_SLICE(buf_slice(c->diagbuf));

  #undef APPEND_SLICE
  #undef APPEND_STR

  return ok ? 0 : ErrNoMem;
}


err_t compiler_configure(compiler_t* c, const char* triple, slice_t buildroot) {
  configure_target(c, triple);
  configure_buildroot(c, buildroot);
  err_t err = configure_cflags(c);
  return err;
}


//————————————————————————————————————————————————————————————————————————————
// name encoding


static bool fqn_recv(const compiler_t* c, buf_t* buf, const type_t* recv) {
  if (recv->kind == TYPE_STRUCT) {
    if (((structtype_t*)recv)->name)
      return buf_print(buf, ((structtype_t*)recv)->name);
  }
  assertf(0,"TODO global variable %s", nodekind_name(recv->kind));
  return buf_printf(buf, "TODO_%s_%s", __FUNCTION__, nodekind_name(recv->kind));
}


static bool fqn_fun(const compiler_t* c, buf_t* buf, const fun_t* fn) {
  if (fn->abi == ABI_CO) {
    buf_print(buf, c->pkgname);
    buf_push(buf, '.');
    if (fn->recvt) {
      fqn_recv(c, buf, fn->recvt);
      buf_push(buf, '.');
    }
  }
  return buf_print(buf, fn->name);
}


bool compiler_fully_qualified_name(const compiler_t* c, buf_t* buf, const node_t* n) {
  // TODO: use n->nsparent when available
  buf_reserve(buf, 32);
  if (n->kind == EXPR_FUN)
    return fqn_fun(c, buf, (fun_t*)n);
  assertf(0,"TODO global variable %s", nodekind_name(n->kind));
  return buf_printf(buf, "TODO_%s_%s", __FUNCTION__, nodekind_name(n->kind));
}


//————————————————————————————————————————————————————————————————————————————
// compiler_compile


// llvm/clang_compile.cc
extern int clang_compile(int argc, const char** argv);


extern void sleep(int);


static char** nullable makeargv(
  compiler_t* c, int* argcp, const char* argv0, ptrarray_t base, ...)
{
  int count = 0;
  usize size = 0;
  va_list ap;

  // calculate size and count
  if (*argv0) {
    count++;
    size += strlen(argv0) + 1;
  }
  for (u32 i = 0; i < base.len; i++) {
    count++;
    assertnotnull(base.v[i]);
    usize len = strlen(base.v[i]);
    size += len + 1*((usize)!!len);
  }
  va_start(ap, base);
  for (const char* arg; (arg = va_arg(ap, const char*)) != NULL; ) {
    count++;
    usize len = strlen(arg);
    size += len + 1*((usize)!!len);
  }
  va_end(ap);

  // allocate memory
  usize arraysize = sizeof(void*) * (usize)count;
  size += arraysize;
  mem_t m = mem_alloc(c->ma, size);
  if (!m.p)
    return NULL;

  int argc = 0;
  char** argv = m.p;
  char* p = m.p + arraysize;

  #define APPEND(str) { \
    const char* str__ = (str); \
    argv[argc++] = p; \
    usize z = strlen(str__) + 1; \
    memcpy(p, str__, z); \
    p += z; \
  }

  // copy strings and build argv array
  if (*argv0)
    APPEND(argv0);
  for (u32 i = 0; i < base.len; i++) {
    if (*(const char*)base.v[i])
      APPEND(base.v[i]);
  }
  va_start(ap, base);
  for (const char* arg; (arg = va_arg(ap, const char*)) != NULL; ) {
    if (*arg)
      APPEND(arg);
  }
  va_end(ap);

  #undef APPEND

  *argcp = argc;
  return argv;
}


static err_t cc_to_asm_main(compiler_t* c, const char* cfile, const char* asmfile) {
  int argc;
  char** argv = makeargv(c, &argc, "clang", c->cflags,
    "-w", // don't produce warnings (already reported by cc_to_obj_main)
    "-S", "-xc", cfile,
    "-o", asmfile,
    NULL);

  if (!argv)
    panic("out of memory");

  dlog("cc %s -> %s", cfile, asmfile);
  int status = clang_compile(argc, (const char**)argv);
  return status == 0 ? 0 : ErrCanceled;
}


static err_t cc_to_obj_main(compiler_t* c, const char* cfile, const char* ofile) {
  // note: clang crashes if we run it more than once in the same process

  int argc;
  char** argv = makeargv(c, &argc, "clang", c->cflags,
    // enable all warnings in debug builds, disable them in release builds
    #if DEBUG
      "-Wall",
      "-Wcovered-switch-default",
      "-Werror=implicit-function-declaration",
      "-Werror=incompatible-pointer-types",
      "-Werror=format-insufficient-args",
      "-Wno-unused-value",
    #else
      "-w",
    #endif
    "-flto=thin",
    "-c", "-xc", cfile,
    "-o", ofile,
    NULL);

  if (!argv)
    panic("out of memory");

  dlog("cc %s -> %s", cfile, ofile);
  int status = clang_compile(argc, (const char**)argv);
  return status == 0 ? 0 : ErrCanceled;
}


static err_t cc_to_obj_async(
  compiler_t* c, subprocs_t* sp, const char* cfile, const char* ofile)
{
  subproc_t* p = subprocs_alloc(sp);
  if (!p)
    return ErrNoMem;
  return subproc_spawn(p, cc_to_obj_main, c, cfile, ofile);
}


static err_t cc_to_asm_async(
  compiler_t* c, subprocs_t* sp, const char* cfile, const char* ofile)
{
  subproc_t* p = subprocs_alloc(sp);
  if (!p)
    return ErrNoMem;

  buf_t asmfile = buf_make(c->ma);
  buf_append(&asmfile, ofile, strlen(ofile) - 1);
  buf_print(&asmfile, "S");
  if (!buf_nullterm(&asmfile))
    return ErrNoMem;

  return subproc_spawn(p, cc_to_asm_main, c, cfile, asmfile.chars);
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


#if DEBUG
  #define DUMP_AST() { \
    dlog("————————— AST —————————"); \
    if ((err = dump_ast((node_t*)unit))) \
      goto end_parser; \
  }
  #define PRINT_AST() DUMP_AST()
#else
  #define DUMP_AST()  ((void)0)
  #define PRINT_AST() if (c->opt_printast) { dump_ast((node_t*)unit); }
#endif


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
  if (c->errcount > errcount) {
    PRINT_AST();
    err = ErrCanceled;
    goto end_parser;
  }
  DUMP_AST();

  // dlog("abort");abort(); // XXX

  // typecheck
  dlog("————————— typecheck —————————");
  if (( err = typecheck(&parser, unit) ))
    goto end_parser;
  PRINT_AST();
  if (c->errcount > errcount) {
    err = ErrCanceled;
    goto end_parser;
  }

  // analyze (ir)
  dlog("————————— analyze —————————");
  memalloc_t ir_ma = ast_ma;
  if (( err = analyze(c, unit, ir_ma) )) {
    dlog("analyze: err=%s", err_str(err));
    goto end_parser;
  }
  if (c->errcount > errcount) {
    err = ErrCanceled;
    goto end_parser;
  }


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


#if 0
  static err_t fmt_ofile(compiler_t* c, input_t* input, buf_t* ofile) {
    u8 sha256[32];
    usize needlen = strlen(c->pkgbuilddir) + 1 + sizeof(sha256)*2 + 2; // pkgbuilddir/sha256.o
    for (;;) {
      ofile->len = 0;
      if (!buf_reserve(ofile, needlen))
        return ErrNoMem;
      abuf_t s = abuf_make(ofile->p, ofile->cap);
      abuf_str(&s, c->pkgbuilddir);
      abuf_c(&s, PATH_SEPARATOR);

      // compute SHA-256 checksum of input file
      sha256_data(sha256, input->data.p, input->data.size);
      abuf_reprhex(&s, sha256, sizeof(sha256), /*spaced*/false);

      abuf_str(&s, ".o");
      usize n = abuf_terminate(&s);
      if (n < needlen) {
        ofile->len = n;
        return 0;
      }
      needlen = n+1;
    }
  }
#else
  static err_t fmt_ofile(compiler_t* c, input_t* input, buf_t* ofile) {
    // {pkgbuilddir}/{input_basename}.o
    // note that pkgbuilddir includes pkgname
    buf_clear(ofile);
    buf_print(ofile, c->pkgbuilddir);
    buf_push(ofile, PATH_SEPARATOR);

    isize p = slastindexof(input->name, PATH_SEPARATOR);
    buf_print(ofile, p != -1 ? &input->name[p + 1] : input->name);
    buf_print(ofile, ".o");

    return buf_nullterm(ofile) ? 0 : ErrNoMem;
  }
#endif


err_t compiler_compile(compiler_t* c, promise_t* promise, input_t* input, buf_t* ofile) {
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
    if (!buf_append(&cfile, ofile->p, ofile->len + 1)) {
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

  if (err)
    goto end;

  // create subprocs attached to promise
  subprocs_t* subprocs = subprocs_create_promise(c->ma, promise);
  if (!subprocs) {
    err = ErrNoMem;
    goto end;
  }

  // compile C -> object
  err = cc_to_obj_async(c, subprocs, cfile.chars, ofile->chars);
  if (err) {
    subprocs_cancel(subprocs);
    goto end;
  }

  if (c->opt_genasm) {
    err = cc_to_asm_async(c, subprocs, cfile.chars, ofile->chars);
    if (err) {
      subprocs_cancel(subprocs);
      goto end;
    }
  }

end:
  buf_dispose(&cfile);
  return err;
}
