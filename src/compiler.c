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


static err_t configure_target(compiler_t* c, const compiler_config_t* config) {
  c->target = *config->target;
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

  if (c->target.sys == SYS_none) {
    c->opt_nolibc = true;
    c->opt_nolibcxx = true;
  }

  c->ldname = target_linker_name(&c->target);

  if (target_is_riscv(&c->target) && isatty(STDOUT_FILENO)) {
    elog("%s: warning: RISC-V support is experimental", coprogname);
  } else if (target_is_arm(&c->target) && isatty(STDOUT_FILENO)) {
    elog("%s: warning: ARM support is experimental", coprogname);
  }

  // enable LTO for optimized builds.
  // RISC-V is disabled because lld fails with float ABI errors.
  // ARM is disabled becaues lld crashes when trying to LTO link.
  if (c->buildmode == BUILDMODE_OPT &&
      !config->nolto &&
      !target_is_riscv(&c->target) &&
      !target_is_arm(&c->target))
  {
    c->lto = 2;
  } else {
    c->lto = 0;
  }

  return 0;
}


static err_t configure_sysroot(compiler_t* c) {
  // sysroot = cocachedir "/" target "-lto"? "-debug"?
  str_t sysroot = str_make(cocachedir);
  bool ok = str_push(&sysroot, PATH_SEP);
  if (( ok &= str_ensure_avail(&sysroot, TARGET_FMT_BUFCAP) ))
    sysroot.len += target_fmt(&c->target, str_end(sysroot), TARGET_FMT_BUFCAP);
  if (c->lto > 0)
    ok &= str_append(&sysroot, "-lto");
  if (c->buildmode == BUILDMODE_DEBUG)
    ok &= str_append(&sysroot, "-debug");
  if (!ok)
    return ErrNoMem;

  mem_freecstr(c->ma, c->sysroot);
  c->sysroot = mem_strdup(c->ma, str_slice(sysroot), 0);
  str_free(sysroot);
  return c->sysroot ? 0 : ErrNoMem;
}


static const char* buildmode_name(buildmode_t m) {
  switch ((enum buildmode)m) {
    case BUILDMODE_DEBUG: return "debug";
    case BUILDMODE_OPT:   return "opt";
  }
  return "unknown";
}


static err_t configure_builddir(compiler_t* c, const compiler_config_t* config) {
  // builddir    = {buildroot}/{mode}-{target}
  // pkgbuilddir = {builddir}/{pkgname}.pkg

  mem_freecstr(c->ma, c->buildroot);
  if (!( c->buildroot = path_abs(config->buildroot).p ))
    return ErrNoMem;

  char targetstr[TARGET_FMT_BUFCAP];
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


static err_t configure_cflags(compiler_t* c) {
  strlist_dispose(&c->cflags);

  // flags used for all C and assembly compilation
  c->cflags = strlist_make(c->ma);
  strlist_addf(&c->cflags, "-B%s", coroot);
  strlist_addf(&c->cflags, "--target=%s", c->target.triple);
  strlist_addf(&c->cflags, "--sysroot=%s/", c->sysroot);
  strlist_addf(&c->cflags, "-resource-dir=%s/clangres/", coroot);
  strlist_add(&c->cflags, "-nostdlib");
  if (c->target.sys == SYS_macos)
    strlist_add(&c->cflags, "-Wno-nullability-completeness");
  if (c->lto)
    strlist_add(&c->cflags, "-flto=thin");

  // RISC-V has a bunch of optional features
  // https://gcc.gnu.org/onlinedocs/gcc/RISC-V-Options.html
  if (c->target.arch == ARCH_riscv64) {
    // e       RV32E (16 gp regs instead of 32)
    // i       RV64
    // a       Atomic Instructions
    // c       Compressed Instructions
    // f       Single-Precision Floating-Point
    // d       Double-Precision Floating-Point (requires f)
    // m       Integer Multiplication and Division
    // v       Vector Extension for Application Processors
    //         (requires d, zve64d, zvl128b)
    // relax   Enable Linker relaxation
    // zvl32b  Minimum Vector Length 32
    // zvl64b  Minimum Vector Length 64 (requires zvl32b)
    // zvl128b Minimum Vector Length 128 (requires zvl64b)
    // zve32x  Vector Extensions for Embedded Processors with maximal 32 EEW
    //         (requires zvl32b)
    // zve32f  Vector Extensions for Embedded Processors with maximal 32 EEW
    //         and F extension. (requires zve32x)
    // zve64x  Vector Extensions for Embedded Processors with maximal 64 EEW
    //         (requires zve32x, zvl64b)
    // zve64f  Vector Extensions for Embedded Processors with maximal 64 EEW
    //         and F extension. (requires zve32f, zve64x)
    // zve64d  Vector Extensions for Embedded Processors with maximal 64 EEW,
    //         F and D extension. (requires zve64f)
    // ... AND A LOT MORE ...
    strlist_add(&c->cflags,
      "-march=rv64iafd",
      "-mabi=lp64d", // ilp32d for riscv32
      "-mno-relax",
      "-mno-save-restore");
  } else if (target_is_arm(&c->target)) {
    strlist_add(&c->cflags,
      "-march=armv6",
      "-mfloat-abi=hard",
      "-mfpu=vfp");
  }

  // end of common flags
  u32 flags_common_end = c->cflags.len;

  // ————— start of common cflags —————
  if (c->buildmode == BUILDMODE_OPT)
    strlist_add(&c->cflags, "-D_FORTIFY_SOURCE=2");
  if (c->target.sys == SYS_none) {
    // invariant: c->opt_nostdlib=true (when c->target.sys == SYS_none)
    strlist_add(&c->cflags,
      "-nostdinc",
      "-ffreestanding");
    // note: must add <resdir>/include explicitly when -nostdinc is set
    strlist_addf(&c->cflags, "-isystem%s/clangres/include", coroot);
  }

  // end of common cflags
  u32 cflags_common_end = c->cflags.len;

  // ————— start of cflags_sysinc —————
  // macos assumes constants in TargetConditionals.h are predefined
  if (c->target.sys == SYS_macos && !c->opt_nolibc)
    strlist_add(&c->cflags, "-include", "TargetConditionals.h");

  // end of cflags_sysinc
  u32 cflags_sysinc_end = c->cflags.len;

  // ————— start of cflags (for compis builds) —————
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
  strlist_addf(&c->cflags, "-isystem%s/co", coroot);

  // end of cflags

  const char*const* argv = (const char*const*)strlist_array(&c->cflags);
  c->flags_common = (slice_t){ .strings = argv, .len = (usize)flags_common_end };
  c->cflags_common = (slice_t){ .strings = argv, .len = (usize)cflags_common_end };
  c->cflags_sysinc = (slice_t){
    .strings = &argv[cflags_common_end],
    .len = (usize)(cflags_sysinc_end - cflags_common_end),
  };

  return c->cflags.ok ? 0 : ErrNoMem;
}


err_t compiler_set_sysroot(compiler_t* c, const char* sysroot) {
  mem_freecstr(c->ma, c->sysroot);
  if (( c->sysroot = mem_strdup(c->ma, slice_cstr(sysroot), 0) ) == NULL)
    return ErrNoMem;
  return configure_cflags(c);
}


err_t configure_options(compiler_t* c, const compiler_config_t* config) {
  c->buildmode = config->buildmode;
  c->opt_nolto = config->nolto;
  c->opt_nomain = config->nomain;
  c->opt_printast = config->printast;
  c->opt_printir = config->printir;
  c->opt_genirdot = config->genirdot;
  c->opt_genasm = config->genasm;
  c->opt_verbose = config->verbose;
  c->opt_nolibc = config->nolibc;
  c->opt_nolibcxx = config->nolibcxx;
  return 0;
}


err_t compiler_configure(compiler_t* c, const compiler_config_t* config) {
  err_t err;
  err = configure_options(c, config);  if (err) return dlog("x"), err;
  err = configure_target(c, config);   if (err) return dlog("x"), err;
  err = configure_builddir(c, config); if (err) return dlog("x"), err;
  err = configure_sysroot(c);          if (err) return dlog("x"), err;
  err = configure_cflags(c);           if (err) return dlog("x"), err;
  return err;
}

//——————————————————————————————————————————————————————————————————————————————————————
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

  // #if DEBUG
  // dlog("spawn_tool:");
  // printf(" ");
  // for (char*const* p = argv; *p; p++) printf(" '%s'", *p);
  // printf("\n");
  // #endif

  if (SPAWN_TOOL_USE_FORK && (flags & SPAWN_TOOL_NOFORK) == 0) {
    const char* cmd = argv[0];
    if (strcmp(cmd, "cc") == 0 || strcmp(cmd, "c++") == 0 ||
        strcmp(cmd, "clang") == 0 || strcmp(cmd, "clang++") == 0 ||
        strcmp(cmd, "as") == 0)
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
    err = fs_writefile(cfile, 0660, buf_slice(g.outbuf));
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
