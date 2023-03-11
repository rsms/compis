// SPDX-License-Identifier: Apache-2.0
#include "colib.h"
#include "compiler.h"
#include "path.h"
#include "abuf.h"
#include "sha256.h"
#include "subproc.h"
#include "llvm/llvm.h"
#include "clang/Basic/Version.inc" // CLANG_VERSION_STRING

#include <err.h>
#include <errno.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

#include <string.h> // XXX


static void set_cstr(memalloc_t ma, char*nullable* dst, slice_t src) {
  mem_freecstr(ma, *dst);
  *dst = mem_strdup(ma, src, 0);
  safecheck(*dst);
}


void compiler_set_pkgname(compiler_t* c, const char* pkgname) {
  set_cstr(c->ma, &c->pkgname, slice_cstr(pkgname));
}


void compiler_init(compiler_t* c, memalloc_t ma, diaghandler_t dh, const char* pkgname) {
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

  if (c->target.sys == SYS_none)
    c->opt_nostdlib = true;

  // set ldname
  switch ((enum target_sys)c->target.sys) {
    case SYS_linux: c->ldname = "ld.lld"; break;
    case SYS_macos: c->ldname = "ld64.lld"; break;
    case SYS_wasi:  c->ldname = "wasm-ld"; break;
    // case SYS_win32: c->ldname = "lld-link"; break;
    case SYS_none:
      c->ldname = "";
      if (c->target.arch == ARCH_wasm32 || c->target.arch == ARCH_wasm64)
        c->ldname = "wasm-ld";
      break;
  }
}


static const char* buildmode_name(buildmode_t m) {
  switch ((enum buildmode)m) {
    case BUILDMODE_DEBUG: return "debug";
    case BUILDMODE_OPT:   return "opt";
  }
  return "unknown";
}


static err_t configure_sysroot(compiler_t* c) {
  // sysroot = {cocachedir}/{target}[-debug]

  char target[80];
  usize targetlen = target_fmt(&c->target, target, sizeof(target));
  if (c->buildmode == BUILDMODE_DEBUG) {
    usize n = strlen("-debug");
    memcpy(&target[targetlen], "-debug", n + 1);
    targetlen += n;
  }

  err_t err;
  char sysroot[PATH_MAX];
  int n = snprintf(sysroot, sizeof(sysroot),
    "%s%c%s", cocachedir, PATH_SEPARATOR, target);
  safecheck(n > 0 && (usize)n < sizeof(sysroot));
  if (( err = fs_mkdirs(sysroot, 0755) ))
    return err;

  if (c->opt_verbose)
    log("using sysroot '%s'", sysroot);

  mem_freecstr(c->ma, c->sysroot);
  if (( c->sysroot = mem_strdup(c->ma, slice_cstr(sysroot), 0) ) == NULL)
    return ErrNoMem;
  return 0;
}


static err_t configure_builddir(compiler_t* c, const char* buildroot) {
  // builddir    = {buildroot}/{mode}-{target}
  // pkgbuilddir = {builddir}/{pkgname}.pkg

  if (!( c->buildroot = path_abs(c->ma, buildroot) ))
    return ErrNoMem;

  char targetstr[64];
  target_fmt(&c->target, targetstr, sizeof(targetstr));
  slice_t target = slice_cstr(targetstr);

  slice_t mode = slice_cstr(buildmode_name(c->buildmode));

  usize len = strlen(c->buildroot) + 1 + mode.len;

  bool isnativetarget = strcmp(llvm_host_triple(), c->target.triple) == 0;
  if (!isnativetarget)
    len += target.len + 1;

  #define APPEND(slice)  memcpy(p, (slice).p, (slice).len), p += (slice.len)

  mem_freecstr(c->ma, c->builddir);
  c->builddir = mem_alloctv(c->ma, char, len + 1);
  if (!c->builddir)
    return ErrNoMem;
  char* p = c->builddir;
  APPEND(slice_cstr(c->buildroot));
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
  c->pkgbuilddir = mem_alloctv(c->ma, char, pkgbuilddir_len + 1);
  if (!c->pkgbuilddir)
    return ErrNoMem;
  p = c->pkgbuilddir;
  APPEND(builddir);
  *p++ = PATH_SEPARATOR;
  APPEND(pkgname);
  APPEND(suffix);
  *p = 0;

  return 0;

  #undef APPEND
}


static err_t add_target_sysdir_if_exists(const char* path, void* cp) {
  compiler_t* c = cp;
  if (fs_isdir(path)) {
    dlog("using sysdir: %s", path);
    strlist_addf(&c->cflags, "-isystem%s", path);
  } else {
    dlog("ignoring non-existent sysdir: %s", path);
  }
  return 0;
}


static err_t configure_cflags(compiler_t* c) {
  strlist_dispose(&c->cflags);

  // flags used for all C and assembly compilation
  c->cflags = strlist_make(c->ma);
  strlist_addf(&c->cflags, "--target=%s", c->target.triple);
  strlist_addf(&c->cflags, "--sysroot=%s/", c->sysroot);
  strlist_addf(&c->cflags, "-resource-dir=%s/clangres/", coroot);
  strlist_add(&c->cflags, "-nostdlib");
  if (c->target.sys == SYS_macos)
    strlist_add(&c->cflags, "-Wno-nullability-completeness");

  // end of common flags
  u32 flags_common_end = c->cflags.len;
  // start of common cflags

  if (c->target.sys == SYS_none) {
    // invariant: c->opt_nostdlib=true (when c->target.sys == SYS_none)
    strlist_add(&c->cflags,
      "-nostdinc",
      "-ffreestanding");
    // note: must add <resdir>/include explicitly when -nostdinc is set
    strlist_addf(&c->cflags, "-isystem%s/clangres/include", coroot);
  }

  if (c->buildmode == BUILDMODE_OPT)
    strlist_add(&c->cflags, "-flto=thin");

  // end of common cflags
  u32 cflags_common_end = c->cflags.len;

  // system-header dirs
  if (!c->opt_nostdlib) {
    target_visit_dirs(&c->target, "sysinc", add_target_sysdir_if_exists, c);
    if (c->target.sys == SYS_linux) {
      strlist_addf(&c->cflags, "-isystem%s/musl/include/%s", coroot, arch_name(c->target.arch));
      strlist_addf(&c->cflags, "-isystem%s/musl/include", coroot);
    }
    if (c->target.sys == SYS_macos) {
      strlist_add(&c->cflags, "-include", "TargetConditionals.h");
    }
  }
  u32 cflags_sysinc_end = c->cflags.len;

  // start of compis cflags

  strlist_add(&c->cflags,
    "-std=c17",
    "-g",
    "-feliminate-unused-debug-types");
  switch ((enum buildmode)c->buildmode) {
    case BUILDMODE_DEBUG:
      strlist_add(&c->cflags, "-O0");
      break;
    case BUILDMODE_OPT:
      strlist_add(&c->cflags, "-O2", "-fomit-frame-pointer");
      break;
  }
  // strlist_add(&c->cflags, "-fPIC");
  strlist_addf(&c->cflags, "-I%s/co", coroot);

  const char*const* argv = (const char*const*)strlist_array(&c->cflags);
  c->sflags_common = (slice_t){ .strings = argv, .len = (usize)flags_common_end };
  c->cflags_common = (slice_t){ .strings = argv, .len = (usize)cflags_common_end };
  c->cflags_sysinc = (slice_t){
    .strings = &argv[cflags_common_end],
    .len = (usize)(cflags_sysinc_end - cflags_common_end),
  };

  return c->cflags.ok ? 0 : ErrNoMem;
}


err_t compiler_configure(compiler_t* c, const target_t* target, const char* buildroot) {
  c->target = *target;
  configure_target(c);
  err_t err = configure_builddir(c, buildroot);
  if (!err) err = configure_sysroot(c);
  if (!err) err = configure_cflags(c);
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
#ifndef SPAWN_TOOL_USE_FORK
  #define SPAWN_TOOL_USE_FORK 1
  // // Note: In my (rsms) tests on macos x86, spawning new processes (posix_spawn)
  // // uses about 10x more memory than fork() in practice, likely from CoW.
  // #if defined(__APPLE__) && defined(DEBUG)
  //   // Note: fork() in debug builds on darwin are super slow, likely because of msan,
  //   // so we disable fork()ing in those cases.
  //   #define SPAWN_TOOL_USE_FORK 0
  // #else
  //   #define SPAWN_TOOL_USE_FORK 1
  // #endif
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
    if (strcmp(cmd, "cc") == 0 || strcmp(cmd, "c++") == 0 ||
        strcmp(cmd, "clang") == 0 || strcmp(cmd, "as") == 0)
    {
      return subproc_fork(p, clang_fork, cwd, argv);
    }
  }
  return subproc_spawn(p, coexefile, argv, /*envp*/NULL, cwd);
}


err_t compiler_spawn_tool_p(
  compiler_t* c, subproc_t* p, strlist_t* args, const char* nullable cwd)
{
  char* const* argv = strlist_array(args);
  if (!args->ok)
    return dlog("strlist_array failed"), ErrNoMem;
  int flags = 0;
  return spawn_tool(p, argv, cwd, flags);
}


err_t compiler_spawn_tool(
  compiler_t* c, subprocs_t* procs, strlist_t* args, const char* nullable cwd)
{
  subproc_t* p = subprocs_alloc(procs);
  if (!p)
    return dlog("subprocs_alloc failed"), ErrCanceled;
  return compiler_spawn_tool_p(c, p, args, cwd);
}


err_t compiler_run_tool_sync(compiler_t* c, strlist_t* args, const char* nullable cwd) {
  subproc_t p = {0};
  err_t err = compiler_spawn_tool_p(c, &p, args, cwd);
  if (err)
    return err;
  return subproc_await(&p);
}


//————————————————————————————————————————————————————————————————————————————
// compiler_compile

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

  // dlog("cc %s -> %s", relpath(cfile), relpath(ofile));
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
