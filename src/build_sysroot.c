// SPDX-License-Identifier: Apache-2.0
#include "colib.h"
#include "compiler.h"
#include "cbuild.h"
#include "strlist.h"
#include "path.h"
#include "bgtask.h"
#include "lockfile.h"
#include "llvm/llvm.h"

#include "syslib_librt.h"
#include "syslib_musl.h"
#include "syslib_wasi.h"
#include "syslib_libcxx.h"
#include "syslib_libcxxabi.h"
#include "syslib_libunwind.h"

#include <string.h>


// CXX_HEADER_INSTALL_DIR: install directory for C++ headers, relative to sysroot
#define CXX_HEADER_INSTALL_DIR  \
  "include" PATH_SEP_STR "c++" PATH_SEP_STR "v" CO_STRX(CO_LIBCXX_ABI_VERSION)


static err_t copy_target_layer_dirs(
  compiler_t* c, bgtask_t* task, const char* src_basedir, const char* dst_basedir)
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
        relpath(src_basedir), path_base(layers[i]), dst_basedir);
      // dlog("copy %s -> %s", relpath(layers[i]), relpath(dstpath.p));
      if (( err = fs_copyfile(layers[i], dstpath.p, 0) ))
        break;
    }
  }

  if UNLIKELY(nlayers_found == 0) {
    elog("error: no layers found in %s/ for target %s",
      path_dir_alloca(layers[0]), path_base(layers[0]));
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
        case SYS_none:  goto bad;
      }
      break;
  }
bad:
  safefail("bad syslib_t");
  return "bad";
}


void syslib_path(compiler_t* c, char buf[PATH_MAX], syslib_t lib) {
  const char* filename = syslib_filename(&c->target, lib);
  UNUSED int n = snprintf(buf, PATH_MAX, "%s/lib/%s", c->sysroot, filename);
  safecheck(n < PATH_MAX);
}


#define FIND_SRCLIST(target, srclist) \
  ((__typeof__(srclist[0])*)_find_srclist( \
    target, srclist, sizeof(srclist[0]), countof(srclist)))


static const void* _find_srclist(
  target_t* t, const void* listp, usize stride, usize listc)
{
  for (usize i = 0; i < listc; i++) {
    const targetdesc_t* t2 = listp + i*stride;
    if (t->arch == t2->arch && t->sys == t2->sys && strcmp(t->sysver, t2->sysver) == 0)
      return t2;
  }
  assert(listc > 0);
  safefail("no impl");
  return listp;
}


static err_t build_libc_musl(compiler_t* c) {
  cbuild_t build;
  cbuild_init(&build, c, "libc");
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
    "-fomit-frame-pointer",
    "-fno-unwind-tables",
    "-fno-asynchronous-unwind-tables",
    "-ffunction-sections",
    "-fdata-sections" );
  strlist_addf(&build.cc, "-Iarch/%s", arch_name(c->target.arch));
  strlist_add(&build.cc,  "-Iarch/generic",
                          "-Isrc/include",
                          "-Isrc/internal");
  // strlist_addf(&build.cc, "-Iinclude-arch/%s", arch_name(c->target.arch));
  // strlist_add(&build.cc,  "-Iinclude");
  strlist_addf(&build.cc, "-isystem%s/include", c->sysroot);

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
  bgtask_t* task = bgtask_start(c->ma, "libc", njobs, 0);

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
    str_free(srcdirs[i]);
  }
  if (c->target.arch != ARCH_any)
    str_free(srcdirs[1]);
  str_free(dstdir);

  // build library
  if (!err) {
    char outfile[PATH_MAX];
    syslib_path(c, outfile, SYSLIB_C);
    err = cbuild_build(&build, outfile, task);
  }

  bgtask_end(task, "");

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
// static err_t build_wasi_libgetpid(compiler_t* c) {
//   ...
//   return 0;
// }


static err_t build_libc_wasi(compiler_t* c) {
  cbuild_t build;
  cbuild_init(&build, c, "libc");
  build.srcdir = path_join_alloca(coroot, "wasi");
  err_t err = 0;
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
  char outfile[PATH_MAX];
  syslib_path(c, outfile, SYSLIB_C);
  err = cbuild_build(&build, outfile, NULL);

  cbuild_dispose(&build);
  return err;
}


static err_t build_libc_darwin(compiler_t* c) {
  // just copy .tbd files and symlinks
  bgtask_t* task = bgtask_start(c->ma, "libc", 0, 0);
  err_t err = copy_target_layer_dirs(c, task, "darwin", "lib");
  bgtask_end(task, "");
  return err;
}


static err_t build_libc(compiler_t* c) {
  switch ((enum target_sys)c->target.sys) {
    case SYS_macos:
      return build_libc_darwin(c);
    case SYS_linux:
      return build_libc_musl(c);
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


static err_t build_librt(compiler_t* c) {
  if (!target_has_syslib(&c->target, SYSLIB_RT))
    return 0;

  cbuild_t build;
  cbuild_init(&build, c, "librt");
  build.srcdir = path_join_alloca(coroot, "librt");

  // see compiler-rt/lib/builtins/CMakeLists.txt
  const char* common_flags[] = {
    "-Os",
    "-fPIC",
    "-fno-builtin",
    "-fomit-frame-pointer",
    "-fvisibility=hidden",
    c->buildmode == BUILDMODE_OPT ? "-flto=thin" : "-g",
    // (c->target.arch == ARCH_riscv32) ? "-fforce-enable-int128" : "",
  };
  strlist_add_array(&build.as, common_flags, countof(common_flags));
  strlist_add_array(&build.cc, common_flags, countof(common_flags));

  strlist_add(&build.cc, "-std=c11");

  strlist_addf(&build.as, "-I%s", build.srcdir);
  strlist_addf(&build.cc, "-I%s", build.srcdir);
  strlist_add_array(&build.cc, c->cflags_sysinc.strings, c->cflags_sysinc.len);

  // TODO: cmake COMPILER_RT_HAS_FCF_PROTECTION_FLAG
  // if (compiler_accepts_flag_for_target("-fcf-protection=full"))
  //   strlist_add(&build.cc, "-fcf-protection=full")

  // TODO: cmake COMPILER_RT_HAS_ASM_LSE
  // if (compiles("asm(\".arch armv8-a+lse\");asm(\"cas w0, w1, [x2]\");"))
  //   strlist_add(&build.cc, "-DHAS_ASM_LSE=1");

  // TODO: cmake COMPILER_RT_HAS_FLOAT16
  // if (compiles("_Float16 f(_Float16 x){return x;}"))
  //   strlist_add(&build.cc, "-DCOMPILER_RT_HAS_FLOAT16=1");

  // strlist_add(&build.cc, c->buildmode == BUILDMODE_OPT ? "-flto=thin" : "-g");
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
      if (c->target.arch == ARCH_aarch64 && strcmp(srcfile, "aarch64/lse.S") == 0) {
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

  char outfile[PATH_MAX];
  syslib_path(c, outfile, SYSLIB_RT);
  err = cbuild_build(&build, outfile, NULL);

end:
  cbuild_dispose(&build);
  return err;
}


static err_t build_libunwind(compiler_t* c) {
  // WASI does not support exceptions
  if (c->target.sys == SYS_wasi)
    return 0;

  cbuild_t build;
  cbuild_init(&build, c, "libunwind");
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
    "-flto=thin",
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

  strlist_add_array(&build.cc, c->cflags_sysinc.strings, c->cflags_sysinc.len);
  strlist_add_array(&build.cxx, c->cflags_sysinc.strings, c->cflags_sysinc.len);

  // add sources
  // ma: temporary memory allocator for storing source filenames
  memalloc_t ma = memalloc_bump_in(c->ma, countof(libunwind_sources)*(PATH_MAX+1), 0);
  { memalloc_ctx_set_scope(ma);
    for (u32 i = 0; i < countof(libunwind_sources); i++) {
      char* srcfile = safechecknotnull(path_join("src", libunwind_sources[i]).p);
      cbuild_add_source(&build, srcfile);
    }
  }

  // build library
  char outfile[PATH_MAX];
  syslib_path(c, outfile, SYSLIB_UNWIND);
  err_t err = cbuild_build(&build, outfile, NULL);

  memalloc_bump_in_dispose(ma);
  cbuild_dispose(&build);
  return err;
}


static err_t build_cxx_config_site(compiler_t* c) {
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


static err_t build_libcxxabi(compiler_t* c) {
  cbuild_t build;
  cbuild_init(&build, c, "libc++abi");
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
    "-flto=thin",
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

  strlist_add_array(&build.cc, c->cflags_sysinc.strings, c->cflags_sysinc.len);
  strlist_add_array(&build.cxx, c->cflags_sysinc.strings, c->cflags_sysinc.len);

  // add sources
  // ma: temporary memory allocator for storing source filenames
  memalloc_t ma = memalloc_bump_in(c->ma, countof(libcxx_sources)*(PATH_MAX+1), 0);
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
  char outfile[PATH_MAX];
  syslib_path(c, outfile, SYSLIB_CXXABI);
  err_t err = cbuild_build(&build, outfile, NULL);

  memalloc_bump_in_dispose(ma);
  cbuild_dispose(&build);
  return err;
}


static err_t build_libcxx(compiler_t* c) {
  cbuild_t build;
  cbuild_init(&build, c, "libc++");
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
    "-flto=thin",
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

  strlist_add_array(&build.cc, c->cflags_sysinc.strings, c->cflags_sysinc.len);
  strlist_add_array(&build.cxx, c->cflags_sysinc.strings, c->cflags_sysinc.len);

  // add sources
  // ma: temporary memory allocator for storing source filenames
  memalloc_t ma = memalloc_bump_in(c->ma, countof(libcxx_sources)*(PATH_MAX+1), 0);
  { memalloc_ctx_set_scope(ma);
    for (u32 i = 0; i < countof(libcxx_sources); i++) {
      if (c->target.sys == SYS_wasi && str_startswith(libcxx_sources[i], "filesystem/"))
        continue; // skip
      char* srcfile = safechecknotnull(path_join("src", libcxx_sources[i]).p);
      cbuild_add_source(&build, srcfile);
    }
  }

  // build library
  char outfile[PATH_MAX];
  syslib_path(c, outfile, SYSLIB_CXX);
  err_t err = cbuild_build(&build, outfile, NULL);

  memalloc_bump_in_dispose(ma);
  cbuild_dispose(&build);
  return err;
}


static err_t copy_sysinc_headers(compiler_t* c) {
  if (c->target.sys == SYS_none)
    return 0;
  bgtask_t* task = bgtask_start(c->ma, "sysinc", 0, 0);
  err_t err = copy_target_layer_dirs(c, task, "sysinc", "include");
  bgtask_end(task, "");
  return err;
}


#define BUILDMARK_FILE_SUFFIX ".buildmark"

static bool must_build(compiler_t* c, const char* component) {
  char* name = strcat_alloca(component, BUILDMARK_FILE_SUFFIX);
  char* filename = path_join_alloca(c->sysroot, name);
  return !fs_isfile(filename);
}

static err_t mark_built_ok(compiler_t* c, const char* component) {
  char* name = strcat_alloca(component, BUILDMARK_FILE_SUFFIX);
  char* filename = path_join_alloca(c->sysroot, name);
  return fs_touch(filename, 0644);
}


static err_t acquire_build_lock(compiler_t* c, lockfile_t* lock) {
  err_t err = 0;
  long lockee_pid;
  char lockfile[PATH_MAX];

  // IMPORTANT: the lock file must be stored in a directory which is guaranteed
  // not to disappear or be moved while the lock is held.
  int n = snprintf(lockfile, sizeof(lockfile),
    "%s%c%s-build.lock", cocachedir, PATH_SEP, path_base(c->sysroot));
  safecheck(n < (int)sizeof(lockfile));

  if ((err = fs_mkdirs(cocachedir, 0755, 0)) && err != ErrExists) {
    elog("failed to create directory '%s': %s", cocachedir, err_str(err));
    return err;
  }

  if (( err = lockfile_trylock(lock, lockfile, &lockee_pid) )) {
    if (err != ErrExists) {
      elog("lockfile_trylock '%s' failed: %s", lockfile, err_str(err));
      return err;
    }
    // note: lockee_pid may be -1 if lockee took a long time to write() its pid
    log("waiting for compis (pid %ld) to finish...", lockee_pid);
    if (( err = lockfile_lock(lock, lockfile) )) {
      elog("lockfile_lock '%s' failed: %s", lockfile, err_str(err));
      return err;
    }
  }
  return 0;
}


err_t build_sysroot_if_needed(compiler_t* c, int flags) {
  // note: this function may be called by multiple processes at once
  bool build_cxx = flags & SYSROOT_BUILD_LIBCXX;
  if (!must_build(c, "base") && (!build_cxx || !must_build(c, "libcxx")) ) {
    // up to date
    return 0;
  }

  // coordinate with other racing processes using an exclusive file-based lock
  lockfile_t lock;
  err_t err = acquire_build_lock(c, &lock);
  if (err)
    return err;

  // note: must check again after locking, in case another process "won"

  if (must_build(c, "base")) {
    // wipe any existing sysroot
    if ((err = fs_remove(c->sysroot)) && err == ErrNotFound)
      err = 0;
    if (!err) err = fs_mkdirs(c->sysroot, 0755, 0);
    if (!err) err = copy_sysinc_headers(c);
    if (!err) err = build_libc(c);
    if (!err) err = build_librt(c);
    if (!err) err = mark_built_ok(c, "base");
  }

  if (build_cxx && must_build(c, "libcxx")) {
    if (!err) err = build_libunwind(c);
    if (!err) err = build_cxx_config_site(c); // __config_site header
    if (!err) err = build_libcxxabi(c);
    if (!err) err = build_libcxx(c);
    if (!err) err = mark_built_ok(c, "libcxx");
  }

  lockfile_unlock(&lock);

  return err;
}
