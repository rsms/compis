// SPDX-License-Identifier: Apache-2.0
#include "colib.h"
#include "compiler.h"
#include "path.h"
#include "abuf.h"
#include "sha256.h"
#include "subproc.h"
#include "llvm/llvm.h"
#include "clang/Basic/Version.inc" // CLANG_VERSION_STRING

#include <sys/stat.h>
#include <unistd.h>
#include <err.h>


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
  strlist_dispose(&c->cflags);
  mem_freecstr(c->ma, c->buildroot);
  mem_freecstr(c->ma, c->builddir);
  mem_freecstr(c->ma, c->sysroot);
  mem_freecstr(c->ma, c->pkgbuilddir);
  mem_freecstr(c->ma, c->pkgname);
}


static void set_secondary_pointer_types(compiler_t* c) {
  // "&[u8]" -- slice of u8 array
  memset(&c->u8stype, 0, sizeof(c->u8stype));
  c->u8stype.kind = TYPE_SLICE;
  c->u8stype.flags = NF_CHECKED;
  c->u8stype.size = c->target.ptrsize;
  c->u8stype.align = c->target.ptrsize;
  c->u8stype.elem = type_u8;

  // "type string &[u8]"
  memset(&c->strtype, 0, sizeof(c->strtype));
  c->strtype.kind = TYPE_ALIAS;
  c->strtype.flags = NF_CHECKED;
  c->strtype.size = c->target.ptrsize;
  c->strtype.align = c->target.ptrsize;
  c->strtype.name = sym_str;
  c->strtype.elem = (type_t*)&c->u8stype;
}


static void configure_target(compiler_t* c) {
  switch (c->target.ptrsize) {
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
      assert(c->target.ptrsize <= 8);
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
  // builddir    = {buildroot}/{mode}-{target}
  // pkgbuilddir = {builddir}/{pkgname}.pkg
  // sysroot     = {builddir}/sysroot

  set_cstr(c->ma, &c->buildroot, buildroot);

  char targetstr[64];
  target_fmt(&c->target, targetstr, sizeof(targetstr));
  slice_t target = slice_cstr(targetstr);

  slice_t mode = slice_cstr(buildmode_name(c->buildmode));

  usize len = buildroot.len + 1 + mode.len;

  bool isnativetarget = strcmp(llvm_host_triple(), c->target.triple) == 0;
  if (!isnativetarget)
    len += target.len + 1;

  #define APPEND(slice)  memcpy(p, (slice).p, (slice).len), p += (slice.len)

  mem_freecstr(c->ma, c->builddir);
  c->builddir = safechecknotnull(mem_alloctv(c->ma, char, len + 1));
  char* p = c->builddir;
  APPEND(buildroot);
  *p++ = PATH_SEPARATOR;
  APPEND(mode);
  if (!isnativetarget) {
    *p++ = '-';
    APPEND(target);
  }
  *p = 0;

  // pkgbuilddir
  mem_freecstr(c->ma, c->pkgbuilddir);
  slice_t pkgname = slice_cstr(c->pkgname);
  slice_t suffix = slice_cstr(".pkg");
  slice_t builddir = { .p = c->builddir, .len = len };
  usize pkgbuilddir_len = len + 1 + pkgname.len + suffix.len;
  c->pkgbuilddir = safechecknotnull(mem_alloctv(c->ma, char, pkgbuilddir_len + 1));
  p = c->pkgbuilddir;
  APPEND(builddir);
  *p++ = PATH_SEPARATOR;
  APPEND(pkgname);
  APPEND(suffix);
  *p = 0;

  // sysroot
  mem_freecstr(c->ma, c->sysroot);
  c->sysroot = safechecknotnull(path_join(c->ma, c->builddir, "sysroot"));

  #undef APPEND
}


static bool dir_exists(const char* path) {
  struct stat st;
  if (stat(path, &st) != 0)
    return false;
  return S_ISDIR(st.st_mode);
}


static err_t configure_cflags(compiler_t* c) {
  strlist_dispose(&c->cflags);
  c->cflags = strlist_make(c->ma,
    "-nostdinc",
    "-nostdlib",
    "-ffreestanding",
    "-g",
    "-feliminate-unused-debug-types",
    "-target", c->target.triple);
  strlist_addf(&c->cflags, "--sysroot=%s", c->sysroot);
  strlist_addf(&c->cflags,
    "-resource-dir=%s/deps/llvmbox/lib/clang/" CLANG_VERSION_STRING, coroot);
  // note: must add <resdir>/include explicitly when -nostdinc or -no-builtin is set
  strlist_addf(&c->cflags,
    "-isystem%s/deps/llvmbox/lib/clang/" CLANG_VERSION_STRING "/include", coroot);

  if (c->target.sys == SYS_macos) strlist_add(&c->cflags,
    "-Wno-nullability-completeness",
    "-include", "TargetConditionals.h");

  // system and libc headers dirs
  char targets_dir[PATH_MAX];
  char dir[PATH_MAX];
  char targetstr[64];
  snprintf(targets_dir, sizeof(targets_dir), "%s/deps/llvmbox/targets", coroot);
  target_fmt(&c->target, targetstr, sizeof(targetstr));
  snprintf(dir, sizeof(dir), "%s/%s/include", targets_dir, targetstr);
  if (dir_exists(dir))
    strlist_addf(&c->cflags, "-isystem%s", dir);
  if (*c->target.sysver) {
    snprintf(dir, sizeof(dir), "%s/%s-%s/include",
      targets_dir, arch_name(c->target.arch), sys_name(c->target.sys));
    if (dir_exists(dir))
      strlist_addf(&c->cflags, "-isystem%s", dir);
  }
  snprintf(dir, sizeof(dir), "%s/any-%s/include", targets_dir, sys_name(c->target.sys));
  if (dir_exists(dir))
    strlist_addf(&c->cflags, "-isystem%s", dir);

  // end of common cflags
  int cflags_common_len = c->cflags.len;
  // start of compis cflags

  strlist_add(&c->cflags, "-std=c17");
  switch ((enum buildmode)c->buildmode) {
    case BUILDMODE_DEBUG:
      strlist_add(&c->cflags, "-O0");
      break;
    case BUILDMODE_OPT:
      strlist_add(&c->cflags, "-O2", "-flto=thin", "-fomit-frame-pointer");
      break;
  }
  // strlist_add(&c->cflags, "-fPIC");
  strlist_addf(&c->cflags, "-I%s/lib", coroot);

  c->cflags_common = (slice_t){
    .strings = (const char*const*)strlist_array(&c->cflags),
    .len = (usize)cflags_common_len };

  return c->cflags.ok ? 0 : ErrNoMem;
}


err_t compiler_configure(compiler_t* c, const target_t* target, slice_t buildroot) {
  c->target = *target;
  configure_target(c);
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
// spawning tools as subprocesses, e.g. cc

// define SPAWN_TOOL_USE_FORK=1 to use fork() by default
// Note: In my (rsms) tests on macos x86, spawning new processes (posix_spawn)
// uses about 10x more memory than fork() in practice, likely from CoW.
#ifndef SPAWN_TOOL_USE_FORK
  #if defined(__APPLE__) && defined(DEBUG)
    // Note: fork() in debug builds on darwin are super slow, likely because of msan,
    // so we disable fork()ing in those cases.
    #define SPAWN_TOOL_USE_FORK 0
  #else
    #define SPAWN_TOOL_USE_FORK 1
  #endif
#endif


static err_t clang_fork(char*const* restrict argv) {
  int argc = 0;
  for (char*const* p = argv; *p++;)
    argc++;
  return clang_main(argc, argv) ? ErrCanceled : 0;
}


err_t spawn_tool(
  subproc_t* p, char*const* restrict argv, const char* nullable cwd, int flags)
{
  assert(argv[0] != NULL);
  if (SPAWN_TOOL_USE_FORK && (flags & SPAWN_TOOL_NOFORK) == 0) {
    const char* cmd = argv[0];
    if (strcmp(cmd, "cc") == 0 || strcmp(cmd, "c++") == 0 || strcmp(cmd, "clang") == 0)
      return subproc_fork(p, clang_fork, cwd, argv);
  }
  return subproc_spawn(p, coexefile, argv, /*envp*/NULL, cwd);
}


err_t compiler_spawn_tool(
  compiler_t* c, subprocs_t* procs, strlist_t* args, const char* nullable cwd)
{
  char* const* argv = strlist_array(args);
  if (!args->ok)
    return dlog("strlist_array failed"), ErrNoMem;
  subproc_t* p = subprocs_alloc(procs);
  if (!p)
    return dlog("subprocs_alloc failed"), ErrNoMem;
  int flags = 0;
  return spawn_tool(p, argv, cwd, flags);
}


//————————————————————————————————————————————————————————————————————————————
// compiler_compile


char** nullable makeargv(
  memalloc_t           ma,
  int*                 argc_out, // number of arguments in returned array
  mem_t* nullable      mem_out,  // allocated memory region
  const char* nullable argv0,    // first argument
  slice_t              baseargs, // base arguments array
  ... // NULL-terminated arguments. Empty-string args are ignored.
){
  int count = 0;
  usize size = 0;
  va_list ap;

  // calculate size and count
  if (*argv0) {
    count++;
    size += strlen(argv0) + 1;
  }
  for (u32 i = 0; i < baseargs.len; i++) {
    count++;
    assertnotnull(baseargs.strings[i]);
    usize len = strlen(baseargs.strings[i]);
    size += len + 1*((usize)!!len);
  }
  va_start(ap, baseargs);
  for (const char* arg; (arg = va_arg(ap, const char*)) != NULL; ) {
    count++;
    if (*arg) {
      usize len = strlen(arg);
      size += len + 1*((usize)!!len);
    }
  }
  va_end(ap);

  // allocate memory
  usize arraysize = sizeof(void*) * (usize)count;
  size += arraysize;
  mem_t m = mem_alloc(ma, size);
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
  for (u32 i = 0; i < baseargs.len; i++) {
    if (*(const char*)baseargs.strings[i])
      APPEND(baseargs.strings[i]);
  }
  va_start(ap, baseargs);
  for (const char* arg; (arg = va_arg(ap, const char*)) != NULL; ) {
    if (*arg)
      APPEND(arg);
  }
  va_end(ap);

  #undef APPEND

  *argc_out = argc;
  if (mem_out)
    *mem_out = m;
  return argv;
}


static err_t cc_to_asm_main(compiler_t* c, const char* cfile, const char* asmfile) {
  strlist_t args = strlist_make(c->ma, "clang");
  strlist_add_list(&args, &c->cflags);
  strlist_add(&args,
    "-w", // don't produce warnings (already reported by cc_to_obj_main)
    "-S", "-xc", cfile,
    "-o", asmfile);
  char* const* argv = strlist_array(&args);
  if (!args.ok)
    return ErrNoMem;

  dlog("cc %s -> %s", cfile, asmfile);
  int status = clang_main(args.len, argv);
  return status == 0 ? 0 : ErrCanceled;
}


static err_t cc_to_obj_main(compiler_t* c, const char* cfile, const char* ofile) {
  // note: clang crashes if we run it more than once in the same process

  strlist_t args = strlist_make(c->ma, "clang");
  strlist_add_list(&args, &c->cflags);
  strlist_add(&args,
    // enable all warnings in debug builds, disable them in release builds
    #if DEBUG
      "-Wall",
      "-Wcovered-switch-default",
      "-Werror=implicit-function-declaration",
      "-Werror=incompatible-pointer-types",
      "-Werror=format-insufficient-args",
      "-Wno-unused-value",
      "-Wno-tautological-compare" // e.g. "x == x"
    #else
      "-w"
    #endif
  );
  strlist_add(&args,
    "-c", "-xc", cfile,
    "-o", ofile,
    c->opt_verbose ? "-v" : "");

  char* const* argv = strlist_array(&args);
  if (!args.ok)
    return ErrNoMem;

  dlog("cc %s -> %s", cfile, ofile);

  // if (c->opt_verbose) {
  //   for (int i = 0; i < args.len; i++)
  //     fprintf(stderr, &" %s"[i==0], argv[i]);
  //   fprintf(stderr, "\n");
  // }

  int status = clang_main(args.len, argv);
  return status == 0 ? 0 : ErrCanceled;
}


static err_t cc_to_obj_async(
  compiler_t* c, subprocs_t* sp, const char* cfile, const char* ofile)
{
  subproc_t* p = subprocs_alloc(sp);
  if (!p)
    return ErrNoMem;
  return subproc_fork(p, cc_to_obj_main, /*cwd*/NULL, c, cfile, ofile);
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

  return subproc_fork(p, cc_to_asm_main, /*cwd*/NULL, c, cfile, asmfile.chars);
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
  u32 errcount = c->errcount;
  err_t err = 0;
  bool printed_trace_ast = false;

  // create bump allocator for AST
  mem_t ast_mem = mem_alloc_zeroed(c->ma, 1024*1024*100);
  if (ast_mem.p == NULL)
    return ErrNoMem;
  memalloc_t ast_ma = memalloc_bump(ast_mem.p, ast_mem.size, MEMALLOC_STORAGE_ZEROED);

  // create parser
  parser_t parser;
  if (!parser_init(&parser, c)) {
    err = ErrNoMem;
    goto end;
  }

  // parse
  dlog_if(opt_trace_parse, "————————— parse —————————");
  unit_t* unit = parser_parse(&parser, ast_ma, input);
  if (c->errcount > errcount) {
    err = ErrCanceled;
    goto end_parser;
  }
  if (opt_trace_parse && c->opt_printast) {
    dlog("————————— AST after parse —————————");
    printed_trace_ast = true;
    dump_ast((node_t*)unit);
  }

  // typecheck
  dlog_if(opt_trace_typecheck, "————————— typecheck —————————");
  if (( err = typecheck(&parser, unit) ))
    goto end_parser;
  if (c->errcount > errcount) {
    err = ErrCanceled;
    goto end_parser;
  }
  if (opt_trace_typecheck && c->opt_printast) {
    dlog("————————— AST after typecheck —————————");
    printed_trace_ast = true;
    dump_ast((node_t*)unit);
  }

  // dlog("abort");abort(); // XXX

  // analyze (ir)
  dlog_if(opt_trace_ir, "————————— IR —————————");
  memalloc_t ir_ma = ast_ma;
  if (( err = analyze(c, unit, ir_ma) )) {
    dlog("IR analyze: err=%s", err_str(err));
    goto end_parser;
  }
  if (c->errcount > errcount) {
    err = ErrCanceled;
    goto end_parser;
  }

  // print AST, if requested
  if (c->opt_printast) {
    c->opt_printast = false;
    if (printed_trace_ast)
      dlog("————————— AST (final) —————————");
    dump_ast((node_t*)unit);
  }

  // generate C code
  dlog_if(opt_trace_cgen, "————————— cgen —————————");
  cgen_t g;
  if (!cgen_init(&g, c, c->ma))
    goto end_parser;
  err = cgen_generate(&g, unit);
  if (!err) {
    if (opt_trace_cgen) {
      fprintf(stderr, "——————————\n%.*s\n——————————\n",
        (int)g.outbuf.len, g.outbuf.chars);
    }
    dlog("cgen %s -> %s", input->name, cfile);
    err = writefile(cfile, 0660, buf_slice(g.outbuf));
  }
  cgen_dispose(&g);


end_parser:
  if (c->opt_printast) {
    // in case an error occurred, print AST "what we have so far"
    dump_ast((node_t*)unit);
  }
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
