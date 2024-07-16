// SPDX-License-Identifier: Apache-2.0
#include "colib.h"
#include "compiler.h"
#include "cbuild.h"
#include "strlist.h"
#include "path.h"
#include "bgtask.h"
#include "llvm/llvm.h"

#include "syslib_librt.h"
#include "syslib_musl.h"
#include "syslib_wasi.h"
#include "syslib_libcxx.h"
#include "syslib_libcxxabi.h"
#include "syslib_libunwind.h"

#include <string.h>
#include <fcntl.h> // open
#include <unistd.h> // close
#include <stdlib.h>


// CXX_HEADER_INSTALL_DIR: install directory for C++ headers, relative to sysroot
#define CXX_HEADER_INSTALL_DIR  \
  "include" PATH_SEP_STR "c++" PATH_SEP_STR "v" CO_STRX(CO_LIBCXX_ABI_VERSION)


static err_t copy_target_layer_dirs(
  const compiler_t* c, bgtask_t* task, const char* src_basedir, const char* dst_basedir)
{
  err_t err = 0;
  str_t dstpath = path_join(c->sysroot, dst_basedir);

  u32 nlayers;
  char** layers = target_layers(&c->target, c->ma, &nlayers, src_basedir);
  if (!layers) {
    str_free(dstpath);
    return ErrNoMem;
  }

  if (task->ntotal == 0) {
    for (u32 i = nlayers; i--;) {
      if (fs_isdir(layers[i]))
        task->ntotal++;
    }
  }

  u32 nlayers_found = 0;
  for (u32 i = nlayers; i--;) {
    if (fs_isdir(layers[i])) {
      nlayers_found++;
      task->n++;
      bgtask_setstatusf(task, "copy {compis}/%s/%s/ -> {sysroot}/%s/",
        relpath(src_basedir), path_base_cstr(layers[i]), dst_basedir);
      // dlog("copy %s -> %s", relpath(layers[i]), relpath(dstpath.p));
      if (( err = fs_copyfile(layers[i], dstpath.p, 0) ))
        break;
    }
  }

  if UNLIKELY(nlayers_found == 0) {
    elog("error: no layers found in %s/ for target %s",
      path_dir_alloca(layers[0]), path_base_cstr(layers[0]));
    err = ErrNotFound;
  }

  target_layers_free(c->ma, layers, nlayers);
  str_free(dstpath);
  return err;
}


const char* syslib_filename(const target_t* target, syslib_t lib) {
  switch (lib) {
    case SYSLIB_RT:     return "librt.a";
    case SYSLIB_CXX:    return "libc++.a";
    case SYSLIB_CXXABI: return "libc++abi.a";
    case SYSLIB_UNWIND: return "libunwind.a";
    case SYSLIB_C:
      switch ((enum target_sys)target->sys) {
        case SYS_macos: return "libSystem.tbd";
        case SYS_linux: return "libc.a";
        case SYS_wasi:  return "libc.a";
        case SYS_win32: goto bad;
        case SYS_none:  goto bad;
      }
      break;
  }
bad:
  safefail("bad syslib_t");
  return "bad";
}


static str_t lib_install_path(const compiler_t* c, const char* filename) {
  return path_join(c->sysroot, "lib", filename);
}


static str_t syslib_install_path(const compiler_t* c, syslib_t lib) {
  return lib_install_path(c, syslib_filename(&c->target, lib));
}


#define FIND_SRCLIST(target, srclist) \
  ((__typeof__(srclist[0])*)_find_srclist( \
    target, srclist, sizeof(srclist[0]), countof(srclist)))


static const void* _find_srclist(
  const target_t* t, const void* listp, usize stride, usize listc)
{
  char targetbuf[64];
  target_fmt(t, targetbuf, sizeof(targetbuf));
  // log("_find_srclist %s", targetbuf);
  for (usize i = 0; i < listc; i++) {
    const targetdesc_t* td = listp + i*stride;
    // log("  try arch=%s sys=%s sysver=%s",
    //   arch_name(td->arch), sys_name(td->sys), td->sysver);
    if (t->arch == td->arch && t->sys == td->sys && strcmp(t->sysver, td->sysver) == 0)
      return td;
  }
  // not found
  if (t->sys != SYS_none) {
    // fall back to conservative implementation for the arch only
    target_t t2 = *t;
    t2.sys = SYS_none;
    return _find_srclist(&t2, listp, stride, listc);
  }
  assert(listc > 0);
  safefail("no impl");
  return listp;
}


static err_t build_libc_musl(const compiler_t* c) {
  cbuild_t build;
  cbuild_init(&build, c, "libc", /*builddir*/c->sysroot);
  build.srcdir = path_join_alloca(coroot, "musl");
  err_t err = 0;

  // flags for compiling assembly sources
  strlist_add(&build.as,
    "-Wa,--noexecstack",
    "-Os",
    "-pipe" );

  // flags for compiling C sources
  strlist_add(&build.cc,
    "-std=c99",
    "-nostdinc",
    "-ffreestanding",
    "-frounding-math",
    "-Wa,--noexecstack",
    "-w", // disable warnings (TODO: find a way to disable only pedantic warnings)
    "-D_XOPEN_SOURCE=700" );
  strlist_add(&build.cc,
    "-Os",
    "-pipe",
    "-fno-asynchronous-unwind-tables",
    "-ffunction-sections",
    "-fdata-sections" );
  if (c->buildmode == BUILDMODE_OPT) {
    strlist_add(&build.cc,
      "-fomit-frame-pointer",
      "-fno-unwind-tables");
  } else {
    strlist_add(&build.cc,
      "-g",
      "-funwind-tables");
  }
  strlist_addf(&build.cc, "-Iarch/%s", arch_name(c->target.arch));
  strlist_addf(&build.cc, "-Iinclude-arch/%s", arch_name(c->target.arch));
  strlist_add(&build.cc,  "-Iarch/generic",
                          "-Isrc/include",
                          "-Isrc/internal");
  strlist_addf(&build.cc, "-isystem%s/include", c->sysroot);

  if (coverbose > 2)
    strlist_add(&build.cc, "-v");

  // dummy libraries to manufacture
  const char* dummy_lib_filenames[] = {
    "libcrypt.a",
    "libdl.a",
    "libm.a",
    "libpthread.a",
    "libresolv.a",
    "librt.a",
    "libutil.a",
    "libxnet.a",
  };

  if (c->target.arch == ARCH_aarch64 && c->target.sys == SYS_linux && c->lto) {
    // disable LTO for aarch64-linux to work around an issue that causes broken
    // executables where somehow init_have_lse_atomics calling getauxval causes
    // PC to jump to 0x0. Here's a backtrace from an lldb run:
    //
    // * thread #1, name = 'hello', stop reason = signal SIGSEGV: invalid address
    //   (fault address: 0x0)
    //   * frame #0: 0x0000000000000000
    //     frame #1: 0x000000000022d748 hello`init_have_lse_atomics + 12
    //     frame #2: 0x000000000022d380 hello`libc_start_init + 52
    //     frame #3: 0x000000000022d3fc hello`libc_start_main_stage2.llvm.1697 + 36
    strlist_add(&build.as, "-fno-lto");
    strlist_add(&build.cc, "-fno-lto");
  }

  // add sources
  const musl_srclist_t* srclist = FIND_SRCLIST(&c->target, musl_srclist);
  if (!cobjarray_reserve(&build.objs, c->ma, countof(musl_sources))) {
    err = ErrNoMem;
    goto end;
  }
  for (u32 i = 0; i < countof(srclist->sources)*8; i++) {
    if (!(srclist->sources[i / 8] & ((u8)1u << (i % 8))))
      continue;
    cbuild_add_source(&build, musl_sources[i]);
  }

  // add crt sources
  #define ADD_CRT_SOURCE(NAME, CFLAGS...) { \
    char* srcfile = strcat_alloca("crt/", srclist->NAME); \
    cobj_t* obj = cbuild_add_source(&build, srcfile); \
    obj->flags |= COBJ_EXCLUDE_FROM_LIB; \
    cobj_setobjfilef(&build, obj, "%s/lib/" CO_STRX(NAME) ".o", c->sysroot); \
    strlist_t* cflags = safechecknotnull(cobj_cflags(&build, obj)); \
    strlist_add(cflags, str_endswith(srcfile, ".c") ? "-DCRT" : "", ##CFLAGS); \
  }
  ADD_CRT_SOURCE(crt1)
  ADD_CRT_SOURCE(rcrt1, "-fPIC")
  ADD_CRT_SOURCE(Scrt1, "-fPIC")
  ADD_CRT_SOURCE(crti)
  ADD_CRT_SOURCE(crtn)
  #undef ADD_CRT_SOURCE

  u32 njobs = cbuild_njobs(&build) + 1 + (u32)(c->target.arch != ARCH_any);
  njobs += countof(dummy_lib_filenames);
  bgtask_t* task = bgtask_open(c->ma, "libc", njobs, 0);

  // copy headers
  str_t dstdir = path_join(c->sysroot, "include");
  str_t srcdirs[] = {
    path_join(build.srcdir, "include"),
    path_join(build.srcdir, "include-arch", arch_name(c->target.arch)),
  };
  for (usize i = 0; i < countof(srcdirs) * (usize)(c->target.arch != ARCH_any); i++) {
    task->n++;
    bgtask_setstatusf(task, "copy {compis}%s/ -> {sysroot}%s/",
      srcdirs[i].p + strlen(coroot), dstdir.p + strlen(c->sysroot));
    if (( err = fs_copyfile(srcdirs[i].p, dstdir.p, 0) ))
      break;
  }
  for (usize i = 0; i < countof(srcdirs); i++)
    str_free(srcdirs[i]);
  str_free(dstdir);
  if (err) goto end_early;

  // create dummy libraries
  slice_t dummy_lib_contents = slice_cstr("!<arch>\n");
  for (usize i = 0; i < countof(dummy_lib_filenames); i++) {
    str_t libfile = lib_install_path(c, dummy_lib_filenames[i]);
    task->n++;
    bgtask_setstatusf(task, "create {sysroot}%s", libfile.p + strlen(c->sysroot));
    err = fs_writefile_mkdirs(libfile.p, 0644, dummy_lib_contents);
    str_free(libfile);
    if (err) goto end_early;
  }

  // build library
  str_t libfile = syslib_install_path(c, SYSLIB_C);
  err = cbuild_build(&build, libfile.p, task);
  str_free(libfile);
  if (err) goto end_early;

end_early:
  if (err) {
    bgtask_end(task, "failed: %s", err_str(err));
  } else {
    bgtask_end(task, "");
  }
  bgtask_close(task);

end:
  cbuild_dispose(&build);
  return err;
}


// TODO: build extra WASI libraries, on demand (ie when user does -lwasi-emulated-getpid)
//   libc-printscan-long-double.a
//   libc-printscan-no-floating-point.a
//   libwasi-emulated-getpid.a
//   libwasi-emulated-mman.a
//   libwasi-emulated-process-clocks.a
//   libwasi-emulated-signal.a
//
// static err_t build_wasi_libgetpid(const compiler_t* c) {
//   ...
//   return 0;
// }


static err_t build_libc_wasi(const compiler_t* c) {
  cbuild_t build;
  cbuild_init(&build, c, "libc", /*builddir*/c->sysroot);
  build.srcdir = path_join_alloca(coroot, "wasi");
  // see deps/wasi/Makefile

  strlist_add(&build.cc,
    "-std=gnu17",
    "-DNDEBUG",
    "-fno-trapping-math",
    "-fno-stack-protector",
    "-mthread-model", "single",
    "-w", // silence warnings
    "-DBULK_MEMORY_THRESHOLD=32" );

  strlist_add(&build.as, "-Os");
  strlist_add(&build.cc, "-Os");

  // cflags used for "bottom half", in addition to build.cc
  strlist_t bottom_cflags = strlist_make(c->ma);
  strlist_addf(&bottom_cflags, "-I%s/wasi/headers-bottom", coroot);
  strlist_addf(&bottom_cflags, "-I%s/wasi/cloudlibc/src/include", coroot);
  strlist_addf(&bottom_cflags, "-I%s/wasi/cloudlibc/src", coroot);
  strlist_addf(&bottom_cflags, "-I%s/wasi/musl/src/include", coroot);
  strlist_addf(&bottom_cflags, "-I%s/wasi/musl/src/internal", coroot);

  // cflags used for "top half", in addition to build.cc
  strlist_t top_cflags = strlist_make(c->ma);
  strlist_addf(&top_cflags, "-I%s/wasi/musl/src/include", coroot);
  strlist_addf(&top_cflags, "-I%s/wasi/musl/src/internal", coroot);
  strlist_addf(&top_cflags, "-I%s/wasi/musl/arch/wasm32", coroot);
  strlist_addf(&top_cflags, "-I%s/wasi/musl/arch/generic", coroot);
  strlist_addf(&top_cflags, "-I%s/wasi/headers-top", coroot);

  // libc sources
  for (u32 i = 0; i < countof(wasi_emmalloc_sources); i++) {
    cbuild_add_source(&build, wasi_emmalloc_sources[i]);
  }
  for (u32 i = 0; i < countof(wasi_libc_bottom_sources); i++) {
    cobj_t* obj = cbuild_add_source(&build, wasi_libc_bottom_sources[i]);
    obj->cflags = &bottom_cflags;
    obj->cflags_external = true;
  }
  for (u32 i = 0; i < countof(wasi_libc_top_sources); i++) {
    cobj_t* obj = cbuild_add_source(&build, wasi_libc_top_sources[i]);
    obj->cflags = &top_cflags;
    obj->cflags_external = true;
  }

  // startfiles
  #define ADD_CRT_SOURCE(objfile, srcfile) { \
    cobj_t* obj = cbuild_add_source(&build, srcfile); \
    obj->flags |= COBJ_EXCLUDE_FROM_LIB; \
    cobj_setobjfilef(&build, obj, "%s/lib/" objfile, c->sysroot); \
  }
  ADD_CRT_SOURCE("crt1.o", wasi_crt1_source)
  ADD_CRT_SOURCE("crt1-command.o", wasi_crt1_command_source)
  ADD_CRT_SOURCE("crt1-reactor.o", wasi_crt1_reactor_source)
  #undef ADD_CRT_SOURCE

  // build
  str_t libfile = syslib_install_path(c, SYSLIB_C);
  err_t err = cbuild_build(&build, libfile.p, NULL);
  str_free(libfile);

  cbuild_dispose(&build);
  return err;
}


static err_t build_libc_darwin(const compiler_t* c) {
  // just copy .tbd files and symlinks
  bgtask_t* task = bgtask_open(c->ma, "libc", 0, 0);
  err_t err = copy_target_layer_dirs(c, task, "darwin", "lib");
  bgtask_end(task, "");
  bgtask_close(task);
  return err;
}


static err_t build_libc(const compiler_t* c) {
  switch ((enum target_sys)c->target.sys) {
    case SYS_macos:
      return build_libc_darwin(c);
    case SYS_linux:
      return build_libc_musl(c);
    case SYS_win32:
      // TODO: win32 libc
      break;
    case SYS_wasi:
      return build_libc_wasi(c);
    case SYS_none:
      assert(target_has_syslib(&c->target, SYSLIB_C) == false);
      return 0;
  }
  safefail("target.sys #%u", c->target.sys);
  return ErrInvalid;
}


static err_t librt_add_aarch64_lse_sources(cbuild_t* b) {
  // adapted from compiler-rt/lib/builtins/CMakeLists.txt
  const char* pats[] = { "cas", "swp", "ldadd", "ldclr", "ldeor", "ldset" };
  for (usize i = 0; i < countof(pats); i++) {
    for (u32 sizem = 0; sizem < 5; sizem++) { // 1 2 4 8 16
      for (u32 model = 1; model < 5; model++) { // 1 2 3 4
        const char* pat = pats[i];
        bool is_cas = i == 0; // strcmp(pat, "cas") == 0;
        if (!is_cas && sizem == 4)
          continue;
        cobj_t* obj = cbuild_add_source(b, "aarch64/lse.S");
        if (!obj)
          return ErrNoMem;
        cobj_addcflagf(b, obj, "-DL_%s", pat);
        cobj_addcflagf(b, obj, "-DSIZE=%u", 1u << sizem);
        cobj_addcflagf(b, obj, "-DMODEL=%u", model);
        cobj_setobjfilef(b, obj, "aarch64.lse_%s_%u_%u.o", pat, 1u << sizem, model);
      }
    }
  }
  return 0;
}


static err_t build_librt(const compiler_t* c) {
  if (!target_has_syslib(&c->target, SYSLIB_RT))
    return 0;

  cbuild_t build;
  cbuild_init(&build, c, "librt", /*builddir*/c->sysroot);
  build.srcdir = path_join_alloca(coroot, "librt");

  // see compiler-rt/lib/builtins/CMakeLists.txt
  const char* common_flags[] = {
    "-Os",
    "-fPIC",
    "-fno-builtin",
    "-fomit-frame-pointer",
    "-fvisibility=hidden",
    c->buildmode == BUILDMODE_DEBUG ? "-g" : "",
    // (c->target.arch == ARCH_riscv32) ? "-fforce-enable-int128" : "",
  };
  strlist_add_array(&build.as, common_flags, countof(common_flags));
  strlist_add_array(&build.cc, common_flags, countof(common_flags));

  if (c->target.arch == ARCH_aarch64 && c->lto) {
    strlist_add(&build.as, "-fno-lto");
    strlist_add(&build.cc, "-fno-lto");
  }

  strlist_add(&build.cc, "-std=c11");

  strlist_addf(&build.as, "-I%s", build.srcdir);
  strlist_addf(&build.cc, "-I%s", build.srcdir);
  strlist_add_slice(&build.cc, c->cflags_sysinc);

  // for riscv/int_mul_impl.inc, included by riscv{32,64}/muldi3.S
  if (target_is_riscv(&c->target))
    strlist_add(&build.as, "-Iriscv");

  // TODO: cmake COMPILER_RT_HAS_FCF_PROTECTION_FLAG
  // if (compiler_accepts_flag_for_target("-fcf-protection=full"))
  //   strlist_add(&build.cc, "-fcf-protection=full")

  // TODO: cmake COMPILER_RT_HAS_ASM_LSE
  // if (compiles("asm(\".arch armv8-a+lse\");asm(\"cas w0, w1, [x2]\");"))
  //   strlist_add(&build.cc, "-DHAS_ASM_LSE=1");

  // TODO: cmake COMPILER_RT_HAS_FLOAT16
  // if (compiles("_Float16 f(_Float16 x){return x;}"))
  //   strlist_add(&build.cc, "-DCOMPILER_RT_HAS_FLOAT16=1");

  err_t err = 0;

  // find source list for target
  if (c->target.sys == SYS_none || c->target.sys == SYS_wasi) {
    // add generic sources only
    for (u32 i = 0; i < countof(librt_sources); i++) {
      if (strchr(librt_sources[i], '/'))
        break;
      cbuild_add_source(&build, librt_sources[i]);
    }
  } else {
    const librt_srclist_t* srclist = FIND_SRCLIST(&c->target, librt_srclist);
    if (!cobjarray_reserve(&build.objs, c->ma, countof(librt_sources))) {
      err = ErrNoMem;
      goto end;
    }
    for (u32 i = 0; i < countof(srclist->sources)*8; i++) {
      if (!(srclist->sources[i / 8] & ((u8)1u << (i % 8))))
        continue;
      const char* srcfile = librt_sources[i];
      if (c->target.arch == ARCH_aarch64 && streq(srcfile, "aarch64/lse.S")) {
        // This file is special -- it is compiled many times with different ppc defs
        // to produce different objects for different function signatures.
        // See compiler-rt/lib/builtins/CMakeLists.txt
        if (( err = librt_add_aarch64_lse_sources(&build) ))
          goto end;
      } else {
        cbuild_add_source(&build, srcfile);
      }
    }
  }

  // TODO: cmake COMPILER_RT_HAS_BFLOAT16
  // if (compiles("__bf16 f(__bf16 x) {return x;}")) {
  //   strlist_addf(&srclist, "%s/truncdfbf2.c.o", objdir);
  //   strlist_add(&srclist, "truncdfbf2.c");
  //   strlist_addf(&srclist, "%s/truncsfbf2.c.o", objdir);
  //   strlist_add(&srclist, "truncsfbf2.c");
  // }

  str_t libfile = syslib_install_path(c, SYSLIB_RT);
  err = cbuild_build(&build, libfile.p, NULL);
  str_free(libfile);

end:
  cbuild_dispose(&build);
  return err;
}


static err_t build_libunwind(const compiler_t* c) {
  // WASI does not support exceptions
  if (c->target.sys == SYS_wasi)
    return 0;

  cbuild_t build;
  cbuild_init(&build, c, "libunwind", /*builddir*/c->sysroot);
  build.srcdir = path_join_alloca(coroot, "libunwind");

  const char* common_flags[] = {
    "-fPIC",
    "-Wa,--noexecstack",
    "-fvisibility=hidden",
    "-fvisibility-inlines-hidden",
    "-funwind-tables",
    "-fstrict-aliasing",
    strcat_alloca("-I", build.srcdir, "/include"),
    "-D_LIBUNWIND_IS_NATIVE_ONLY", // only build for current --target
    // TODO: c->singlethreaded ? "-D_LIBUNWIND_HAS_NO_THREADS" : "",
    // TODO: c->singlethreaded ? "-D_LIBCPP_HAS_NO_THREADS" : "",
    // TODO: if c->target.arch == ARCH_arm && c->target.float_abi == FPABI_HARD
    //   "-DCOMPILER_RT_ARMHF_TARGET"
  };
  const char* common_flags_opt[] = {
    "-Os",
    "-DNDEBUG",
  };
  const char* common_flags_debug[] = {
    "-g",
    "-O1",
    "-D_DEBUG",
  };

  strlist_add_array(&build.as, common_flags, countof(common_flags));
  strlist_add_array(&build.cc, common_flags, countof(common_flags));
  strlist_add_array(&build.cxx, common_flags, countof(common_flags));
  if (c->buildmode == BUILDMODE_OPT) {
    strlist_add_array(&build.as, common_flags_opt, countof(common_flags_opt));
    strlist_add_array(&build.cc, common_flags_opt, countof(common_flags_opt));
    strlist_add_array(&build.cxx, common_flags_opt, countof(common_flags_opt));
  } else {
    strlist_add_array(&build.as, common_flags_debug, countof(common_flags_debug));
    strlist_add_array(&build.cc, common_flags_debug, countof(common_flags_debug));
    strlist_add_array(&build.cxx, common_flags_debug, countof(common_flags_debug));
  }

  strlist_add(&build.cc,
    "-std=c11"
  );
  strlist_add(&build.cxx,
    "-std=c++20",
    "-fno-exceptions",
    "-fno-rtti",
    "-nostdlib++",
    "-nostdinc++"
  );
  // strlist_addf(&build.cxx, "-I%s/lib/libcxx/include", coroot);

  strlist_add_slice(&build.cc, c->cflags_sysinc);
  strlist_add_slice(&build.cxx, c->cflags_sysinc);

  // add sources
  // ma: temporary memory allocator for storing source filenames
  usize nsources_space_max = PATH_MAX * (
      countof(libunwind_sources)
    + countof(libunwind_sources_arm)
    + countof(libunwind_sources_apple)
  );
  memalloc_t ma = memalloc_bump_in(c->ma, nsources_space_max, 0);
  { memalloc_ctx_set_scope(ma);
    for (u32 i = 0; i < countof(libunwind_sources); i++) {
      char* srcfile = safechecknotnull(path_join("src", libunwind_sources[i]).p);
      cbuild_add_source(&build, srcfile);
    }
    if (target_is_arm(&c->target)) {
      for (u32 i = 0; i < countof(libunwind_sources_arm); i++) {
        char* srcfile = safechecknotnull(path_join("src", libunwind_sources_arm[i]).p);
        cbuild_add_source(&build, srcfile);
      }
    }
    if (target_is_apple(&c->target)) {
      for (u32 i = 0; i < countof(libunwind_sources_apple); i++) {
        char* srcfile = safechecknotnull(path_join("src", libunwind_sources_apple[i]).p);
        cbuild_add_source(&build, srcfile);
      }
    }
  }

  // build library
  str_t libfile = syslib_install_path(c, SYSLIB_UNWIND);
  err_t err = cbuild_build(&build, libfile.p, NULL);
  str_free(libfile);

  memalloc_bump_in_dispose(ma);
  cbuild_dispose(&build);
  return err;
}


static err_t build_cxx_config_site(const compiler_t* c) {
  err_t err = 0;

  // example of __config_site for x86_64-macos, generated by libcxx cmake:
  //   #define _LIBCPP_ABI_VERSION 1
  //   #define _LIBCPP_ABI_NAMESPACE __1
  //   /* #undef _LIBCPP_ABI_FORCE_ITANIUM */
  //   /* #undef _LIBCPP_ABI_FORCE_MICROSOFT */
  //   /* #undef _LIBCPP_HAS_NO_THREADS */
  //   /* #undef _LIBCPP_HAS_NO_MONOTONIC_CLOCK */
  //   /* #undef _LIBCPP_HAS_MUSL_LIBC */
  //   /* #undef _LIBCPP_HAS_THREAD_API_PTHREAD */
  //   /* #undef _LIBCPP_HAS_THREAD_API_EXTERNAL */
  //   /* #undef _LIBCPP_HAS_THREAD_API_WIN32 */
  //   /* #undef _LIBCPP_HAS_THREAD_LIBRARY_EXTERNAL */
  //   /* #undef _LIBCPP_DISABLE_VISIBILITY_ANNOTATIONS */
  //   #define _LIBCPP_HAS_NO_VENDOR_AVAILABILITY_ANNOTATIONS
  //   /* #undef _LIBCPP_NO_VCRUNTIME */
  //   /* #undef _LIBCPP_TYPEINFO_COMPARISON_IMPLEMENTATION */
  //   /* #undef _LIBCPP_HAS_NO_FILESYSTEM_LIBRARY */
  //   /* #undef _LIBCPP_HAS_PARALLEL_ALGORITHMS */
  //   /* #undef _LIBCPP_HAS_NO_RANDOM_DEVICE */
  //   /* #undef _LIBCPP_HAS_NO_LOCALIZATION */
  //   /* #undef _LIBCPP_HAS_NO_WIDE_CHARACTERS */
  //   #define _LIBCPP_ENABLE_ASSERTIONS_DEFAULT 0
  //   /* #undef _LIBCPP_ENABLE_DEBUG_MODE */
  //

  str_t contents = str_make(""
    "#ifndef _LIBCPP___CONFIG_SITE\n"
    "#define _LIBCPP___CONFIG_SITE\n"
    "\n"
    "#define _LIBCPP_ABI_VERSION " CO_STRX(CO_LIBCXX_ABI_VERSION) "\n"
    "#define _LIBCPP_ABI_NAMESPACE __" CO_STRX(CO_LIBCXX_ABI_VERSION) "\n"
    "#define _LIBCPP_DISABLE_EXTERN_TEMPLATE\n"
    "#define _LIBCPP_DISABLE_NEW_DELETE_DEFINITIONS\n"
    "#define _LIBCPP_DISABLE_VISIBILITY_ANNOTATIONS\n"
    "#define _LIBCPP_ENABLE_CXX17_REMOVED_UNEXPECTED_FUNCTIONS\n"
    "#define _LIBCPP_HAS_NO_PRAGMA_SYSTEM_HEADER\n"
    "#define _LIBCPP_HAS_NO_VENDOR_AVAILABILITY_ANNOTATIONS\n"
    "\n");

  bool ok = contents.len > 0;

  if (c->target.sys == SYS_wasi) {
    ok &= str_append(&contents,
      "#define _LIBCPP_HAS_NO_THREADS\n"
      "#define _LIBCPP_NO_EXCEPTIONS\n");
  }

  if (c->target.sys == SYS_linux || c->target.sys == SYS_wasi)
    ok &= str_append(&contents, "#define _LIBCPP_HAS_MUSL_LIBC\n");

  ok &= str_append(&contents, "#endif // _LIBCPP___CONFIG_SITE\n");

  if (!ok) {
    err = ErrNoMem;
  } else {
    char* path = path_join_alloca(c->sysroot, CXX_HEADER_INSTALL_DIR, "__config_site");
    vlog("creating %s", relpath(path));
    err = fs_writefile(path, 0644, str_slice(contents));
    if (err == ErrNotFound) {
      err = fs_mkdirs(path_dir_alloca(path), 0755, 0);
      if (!err || err == ErrExists)
        err = fs_writefile(path, 0644, str_slice(contents));
    }
  }

  str_free(contents);
  return err;
}


static err_t build_libcxxabi(const compiler_t* c) {
  cbuild_t build;
  cbuild_init(&build, c, "libc++abi", /*builddir*/c->sysroot);
  build.srcdir = path_join_alloca(coroot, "libcxxabi");

  const char* common_flags[] = {
    "-fPIC",
    "-fvisibility=hidden",
    "-fvisibility-inlines-hidden",
    "-funwind-tables",
    "-Wno-user-defined-literals",
    "-faligned-allocation",
    "-fstrict-aliasing",
    strcat_alloca("-I", build.srcdir, "/include"),

    "-D_LIBCXXABI_BUILDING_LIBRARY",
    c->target.sys == SYS_wasi ? "-D_LIBCXXABI_HAS_NO_THREADS" : "",
    "-DLIBCXX_BUILDING_LIBCXXABI",
    "-D_LIBCXXABI_DISABLE_VISIBILITY_ANNOTATIONS",
  };
  const char* common_flags_opt[] = {
    "-Os",
    "-DNDEBUG",
  };
  const char* common_flags_debug[] = {
    "-g",
    "-O1",
    // "-D_LIBCPP_ENABLE_DEBUG_MODE=1",
  };

  strlist_add_array(&build.as, common_flags, countof(common_flags));
  strlist_add_array(&build.cc, common_flags, countof(common_flags));
  strlist_add_array(&build.cxx, common_flags, countof(common_flags));
  if (c->buildmode == BUILDMODE_OPT) {
    strlist_add_array(&build.as, common_flags_opt, countof(common_flags_opt));
    strlist_add_array(&build.cc, common_flags_opt, countof(common_flags_opt));
    strlist_add_array(&build.cxx, common_flags_opt, countof(common_flags_opt));
  } else {
    strlist_add_array(&build.as, common_flags_debug, countof(common_flags_debug));
    strlist_add_array(&build.cc, common_flags_debug, countof(common_flags_debug));
    strlist_add_array(&build.cxx, common_flags_debug, countof(common_flags_debug));
  }

  strlist_add(&build.cc,
    "-std=c11"
  );
  strlist_add(&build.cxx,
    "-std=c++20",
    "-nostdinc++"
  );

  if (c->target.sys == SYS_wasi)
    strlist_add(&build.cxx, "-fno-exceptions");

  strlist_addf(&build.cxx, "-I%s/libcxxabi/include", coroot);
  strlist_addf(&build.cxx, "-I%s/libunwind/include", coroot);
  strlist_addf(&build.cxx, "-I%s/libcxx/include", coroot);
  strlist_addf(&build.cxx, "-I%s/libcxx/src", coroot); // include/atomic_support.h

  strlist_add_slice(&build.cc, c->cflags_sysinc);
  strlist_add_slice(&build.cxx, c->cflags_sysinc);

  // add sources
  // ma: temporary memory allocator for storing source filenames
  memalloc_t ma = memalloc_bump_in(c->ma, countof(libcxx_sources)*PATH_MAX, 0);
  { memalloc_ctx_set_scope(ma);
    for (u32 i = 0; i < countof(libcxxabi_sources); i++) {
      if (c->target.sys == SYS_wasi &&
          ( streq(libcxxabi_sources[i], "cxa_exception.cpp") ||
            streq(libcxxabi_sources[i], "cxa_personality.cpp") ||
            streq(libcxxabi_sources[i], "cxa_thread_atexit.cpp") ))
      {
        // WASM/WASI doesn't support exceptions and is single-threaded.
        // TODO: also exclude "cxa_exception_storage.cpp"?
        continue;
      }
      char* srcfile = safechecknotnull(path_join("src", libcxxabi_sources[i]).p);
      cbuild_add_source(&build, srcfile);
    }
  }

  // build library
  str_t libfile = syslib_install_path(c, SYSLIB_CXXABI);
  err_t err = cbuild_build(&build, libfile.p, NULL);
  str_free(libfile);

  memalloc_bump_in_dispose(ma);
  cbuild_dispose(&build);
  return err;
}


static err_t build_libcxx(const compiler_t* c) {
  cbuild_t build;
  cbuild_init(&build, c, "libc++", /*builddir*/c->sysroot);
  build.srcdir = path_join_alloca(coroot, "libcxx");

  const char* common_flags[] = {
    "-fPIC",
    "-fvisibility=hidden",
    "-fvisibility-inlines-hidden",
    "-funwind-tables",
    "-Wno-user-defined-literals",
    "-faligned-allocation",
    strcat_alloca("-I", build.srcdir, "/include"),

    "-D_LIBCPP_BUILDING_LIBRARY",
    "-DLIBCXX_BUILDING_LIBCXXABI",
    "-D_LIBCXXABI_DISABLE_VISIBILITY_ANNOTATIONS",
  };
  const char* common_flags_opt[] = {
    "-Os",
    "-DNDEBUG",
  };
  const char* common_flags_debug[] = {
    "-g",
    "-O1",
    // "-D_LIBCPP_ENABLE_DEBUG_MODE=1",
  };

  strlist_add_array(&build.as, common_flags, countof(common_flags));
  strlist_add_array(&build.cc, common_flags, countof(common_flags));
  strlist_add_array(&build.cxx, common_flags, countof(common_flags));
  if (c->buildmode == BUILDMODE_OPT) {
    strlist_add_array(&build.as, common_flags_opt, countof(common_flags_opt));
    strlist_add_array(&build.cc, common_flags_opt, countof(common_flags_opt));
    strlist_add_array(&build.cxx, common_flags_opt, countof(common_flags_opt));
  } else {
    strlist_add_array(&build.as, common_flags_debug, countof(common_flags_debug));
    strlist_add_array(&build.cc, common_flags_debug, countof(common_flags_debug));
    strlist_add_array(&build.cxx, common_flags_debug, countof(common_flags_debug));
  }

  strlist_add(&build.cc,
    "-std=c11"
  );
  strlist_add(&build.cxx,
    "-std=c++20",
    "-nostdinc++"
  );

  if (c->target.sys == SYS_wasi)
    strlist_add(&build.cxx, "-fno-exceptions");

  strlist_addf(&build.cxx, "-I%s/libcxx/include", coroot);
  strlist_addf(&build.cxx, "-I%s/libcxxabi/include", coroot);
  strlist_addf(&build.cxx, "-I%s/libcxx/src", coroot);

  strlist_add_slice(&build.cc, c->cflags_sysinc);
  strlist_add_slice(&build.cxx, c->cflags_sysinc);

  // add sources
  // ma: temporary memory allocator for storing source filenames
  memalloc_t ma = memalloc_bump_in(c->ma, countof(libcxx_sources)*PATH_MAX, 0);
  { memalloc_ctx_set_scope(ma);
    for (u32 i = 0; i < countof(libcxx_sources); i++) {
      if (c->target.sys == SYS_wasi &&
          string_startswith(libcxx_sources[i], "filesystem/"))
      {
        continue; // skip
      }
      char* srcfile = safechecknotnull(path_join("src", libcxx_sources[i]).p);
      cbuild_add_source(&build, srcfile);
    }
  }

  // build library
  str_t libfile = syslib_install_path(c, SYSLIB_CXX);
  err_t err = cbuild_build(&build, libfile.p, NULL);
  str_free(libfile);

  memalloc_bump_in_dispose(ma);
  cbuild_dispose(&build);
  return err;
}


static err_t copy_sysinc_headers(const compiler_t* c) {
  if (c->target.sys == SYS_none || c->target.sys == SYS_win32) // TODO: win32
    return 0;
  bgtask_t* task = bgtask_open(c->ma, "sysinc", 0, 0);
  err_t err = copy_target_layer_dirs(c, task, "sysinc", "include");
  if (!err) {
    str_t srcpath = path_join(coroot, "co", "coprelude.h");
    str_t dstpath = path_join(c->sysroot, "include", "coprelude.h");
    err = fs_copyfile(srcpath.p, dstpath.p, 0);
    str_free(dstpath);
    str_free(srcpath);
  }
  bgtask_end(task, "");
  bgtask_close(task);
  return err;
}


#define build_ok_filename_alloca(c, component) \
  strcat_alloca((c)->sysroot, PATH_SEP_STR, (component), ".ok")


static str_t lockfile_path(const compiler_t* c, const char* component) {
  // IMPORTANT: the lock file must be stored in a directory which is guaranteed
  // not to disappear or move while the lock is held.
  str_t filename = {0};
  str_append(&filename, c->sysroot, PATH_SEP_STR, component, ".lock");
  safecheck(filename.p != NULL);
  return filename;
}


__attribute__((__noinline__))
static bool is_component_built(const compiler_t* c, const char* component) {
  char* filename = build_ok_filename_alloca(c, component);
  return fs_isfile(filename);
}


// build_component
// Returns true if this process should go ahead and build the component.
// Returns false if the component is available (another process completed the work.)
// If false is returned, caller should check *errp.
static bool build_component(
  const compiler_t* c, int* lockfdp, err_t* errp, u32 flags, const char* component)
{
  *errp = 0;

  // if the component is installed, no additional work is necessary
  if ((flags & SYSROOT_BUILD_FORCE) == 0 && is_component_built(c, component)) {
    if (coverbose) {
      vlog("%s/%s: up to date",
        coverbose > 1 ? relpath(c->sysroot) : path_base_cstr(c->sysroot), component);
    }
    return false;
  }

  str_t lockfile = lockfile_path(c, component);

  // open lockfile
  int lockfd = open(lockfile.p, O_WRONLY | O_CREAT, 0644);
  if (lockfd < 0) {
    *errp = err_errno();
    elog("%s: open '%s': %s", __FUNCTION__, lockfile.p, err_str(*errp));
    return false;
  }
  *lockfdp = lockfd;

  // try to acquire the lock
  long lockee_pid;
  err_t err = fs_trylock(lockfd, &lockee_pid);
  if (err == 0) {
    bool build = (flags & SYSROOT_BUILD_FORCE) || !is_component_built(c, component);
    if (build) {
      if (coverbose) {
        vlog("%s/%s: building",
          coverbose > 1 ? relpath(c->sysroot) : path_base_cstr(c->sysroot), component);
      }
    } else {
      // race condition; component already built
      fs_unlock(lockfd);
      close(lockfd);
    }
    str_free(lockfile);
    return build;
  }

  if (err == ErrExists) {
    // another process is holding the lock
    // note: lockee_pid may be -1 if lockee took a long time to write() its pid
    // or if the lockfile implementation does not support getting lockee pid.
    if (lockee_pid > -1) {
      log("waiting for compis (pid %ld) to finish...", lockee_pid);
    } else {
      log("waiting for another compis process to finish...");
    }
    // wait for lock
    if (( err = fs_lock(lockfd) )) {
      elog("fs_lock '%s': %s", lockfile.p, err_str(err));
      *errp = err;
    } else {
      fs_unlock(lockfd);
    }
  } else {
    // lockfile_trylock failed (for reason other than lock being held by other thread)
    *errp = err;
    elog("fs_unlock '%s': %s", lockfile.p, err_str(err));
  }

  str_free(lockfile);
  close(lockfd);
  return false;
}


__attribute__((__noinline__))
static void finalize_build_component(
  const compiler_t* c, int lockfd, err_t* errp, const char* component)
{
  str_t lockfile = lockfile_path(c, component);
  if (!*errp) {
    // rename the lockfile to a ".ok" file, marking the build successful
    char* ok_filename = build_ok_filename_alloca(c, component);
    if (rename(lockfile.p, ok_filename) != 0)
      *errp = err_errno();
  }
  str_free(lockfile);
  // unlock the lockfile, resuming any processes waiting for the component
  fs_unlock(lockfd);
  close(lockfd);
}


err_t build_sysroot(const compiler_t* c, u32 flags) {
  // Coordinate with other racing processes using file-based locks
  int lockfd;
  err_t err;

  assertf(c->sysroot, "compiler not configured");

  if (( err = fs_mkdirs(c->sysroot, 0755, 0) )) {
    elog("mkdirs %s: %s", c->sysroot, err_str(err));
    return err;
  }

  if (!err && c->target.sys != SYS_none &&
      build_component(c, &lockfd, &err, flags, "sysinc"))
  {
    err = copy_sysinc_headers(c);
    finalize_build_component(c, lockfd, &err, "sysinc");
  }

  if (!err && target_has_syslib(&c->target, SYSLIB_C) &&
      build_component(c, &lockfd, &err, flags, "libc"))
  {
    err = build_libc(c);
    finalize_build_component(c, lockfd, &err, "libc");
  }

  if (!err && target_has_syslib(&c->target, SYSLIB_RT) &&
      build_component(c, &lockfd, &err, flags, "librt"))
  {
    err = build_librt(c);
    finalize_build_component(c, lockfd, &err, "librt");
  }

  if (!err && (flags & SYSROOT_BUILD_LIBUNWIND) &&
      target_has_syslib(&c->target, SYSLIB_UNWIND) &&
      build_component(c, &lockfd, &err, flags, "libunwind"))
  {
    err = build_libunwind(c);
    finalize_build_component(c, lockfd, &err, "libunwind");
  }

  if (!err && (flags & SYSROOT_BUILD_LIBCXX) &&
      target_has_syslib(&c->target, SYSLIB_CXX) &&
      build_component(c, &lockfd, &err, flags, "libcxx"))
  {
    assert(target_has_syslib(&c->target, SYSLIB_CXXABI));
    if (!err) err = build_cxx_config_site(c); // __config_site header
    if (!err) err = build_libcxxabi(c);
    if (!err) err = build_libcxx(c);
    finalize_build_component(c, lockfd, &err, "libcxx");
  }

  return err;
}


// ———————————————————————————————————————————————————————————————————————————————————
// "build-sysroot" command-line command


// cli options
static bool opt_help = false;
static bool opt_force = false;
static bool opt_debug = false;
static bool opt_print = false;
static bool opt_nolto = false;
static int  opt_verbose = 0; // ignored; we use coverbose
static int  g_target_count = 1;

#define FOREACH_CLI_OPTION(S, SV, L, LV,  DEBUG_L, DEBUG_LV) \
  /* S( var, ch, name,          descr) */\
  /* SV(var, ch, name, valname, descr) */\
  /* L( var,     name,          descr) */\
  /* LV(var,     name, valname, descr) */\
  S( &opt_debug,  'd', "debug",   "Build sysroot for debug mode")\
  S( &opt_force,  'f', "force",   "Build sysroot even when it's up to date")\
  L( &opt_print,       "print",   "Just print the absolute path (don't build)")\
  L( &opt_nolto,       "no-lto",  "Build sysroot without LTO")\
  S( &opt_verbose,'v', "verbose", "Verbose mode prints extra information")\
  S( &opt_help,   'h', "help",    "Print help on stdout and exit")\
// end FOREACH_CLI_OPTION

#include "cliopt.inc.h"


static void command_line_help(const char* cmdname) {
  char tmpbuf[TARGET_FMT_BUFCAP];

  printf(
    "Builds target sysroot (normally done automatically.)\n"
    "Usage: %s %s [options] [<target> ...]\n"
    "Options:\n"
    "",
    coprogname, cmdname);
  cliopt_print();

  target_fmt(target_default(), tmpbuf, sizeof(tmpbuf));
  printf(
    "<target>\n"
    "  Specify what target(s) to build sysroot for.\n"
    "  If no <target> is specified, the host target (%s) is assumed.\n"
    "  Available targets:\n",
    tmpbuf);

  usize maxcol = 80;
  usize col = strlen("    all"); printf("    all");
  for (usize i = 0; i < SUPPORTED_TARGETS_COUNT; i++) {
    target_fmt(&supported_targets[i], tmpbuf, sizeof(tmpbuf));
    col += 1 + strlen(tmpbuf);
    if (col > maxcol) {
      printf("\n    %s", tmpbuf);
      col = 4 + strlen(tmpbuf);
    } else {
      printf(" %s", tmpbuf);
    }
  }
  printf("\n");

  exit(0);
}


static void main_diaghandler(const diag_t* d, void* nullable userdata) {
  // unused
}


static bool build_sysroot_for_target(compiler_t* compiler, const target_t* target) {
  err_t err;
  char tmpbuf[TARGET_FMT_BUFCAP];

  compiler_config_t compiler_config = {
    .target = target,
    .buildroot = "build-THIS-IS-A-BUG-IN-COMPIS", // should never be used
    .buildmode = opt_debug ? BUILDMODE_DEBUG : BUILDMODE_OPT,
    .verbose = coverbose,
    .nolto = opt_nolto,
  };
  if (( err = compiler_configure(compiler, &compiler_config) )) {
    dlog("compiler_configure: %s", err_str(err));
    return false;
  }

  if (opt_print) {
    if (g_target_count > 1) {
      target_fmt(target, tmpbuf, sizeof(tmpbuf));
      printf("%s %s\n", tmpbuf, compiler->sysroot);
    } else {
      printf("%s\n", compiler->sysroot);
    }
    return true;
  }

  target_fmt(target, tmpbuf, sizeof(tmpbuf));
  if (coverbose) {
    vlog("building sysroot for %s at %s", tmpbuf, relpath(compiler->sysroot));
  } else {
    log("building sysroot for %s", tmpbuf);
  }

  u32 flags = SYSROOT_BUILD_LIBC | SYSROOT_BUILD_LIBCXX | SYSROOT_BUILD_LIBUNWIND;
  if (opt_force) flags |= SYSROOT_BUILD_FORCE;
  if (( err = build_sysroot(compiler, flags) )) {
    dlog("build_sysroot: %s", err_str(err));
    return false;
  }

  return true;
}


static bool build_sysroot_for_targetstr(compiler_t* compiler, const char* targetstr) {
  const target_t* target = target_find(targetstr);
  if (!target) {
    elog("Invalid target \"%s\"", targetstr);
    elog("See `%s targets` for a list of supported targets", relpath(coexefile));
    return false;
  }
  return build_sysroot_for_target(compiler, target);
}


int main_build_sysroot(int argc, char* argv[]) {
  if (!cliopt_parse(&argc, &argv, command_line_help))
    return 1;

  compiler_t compiler;
  compiler_init(&compiler, memalloc_default(), &main_diaghandler);

  // if no <target>s are specified, build for the default (host) target
  if (argc == 0) {
    bool ok = build_sysroot_for_target(&compiler, target_default());
    return ok ? 0 : 1;
  }

  // handle special "all" target
  for (int i = 0; i < argc; i++) {
    if (strcmp(argv[i], "all") == 0) {
      g_target_count = SUPPORTED_TARGETS_COUNT;
      for (usize i = 0; i < SUPPORTED_TARGETS_COUNT; i++) {
        if (!build_sysroot_for_target(&compiler, &supported_targets[i]))
          return 1;
      }
      return 0;
    }
  }

  // build for specified targets
  g_target_count = argc;
  for (int i = 0; i < argc; i++) {
    if (!build_sysroot_for_targetstr(&compiler, argv[i]))
      return 1;
  }
  return 0;
}
