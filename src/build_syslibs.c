// SPDX-License-Identifier: Apache-2.0
#include "colib.h"
#include "compiler.h"
#include "cbuild.h"
#include "strlist.h"
#include "path.h"
#include "bgtask.h"
#include "llvm/llvm.h"

#include "librt_info.h"

#include <unistd.h>
#include <libgen.h>
#include <sys/stat.h>
#include <stdlib.h>
#include <string.h>


static char* lib_path(compiler_t* c, char buf[PATH_MAX], const char* filename) {
  UNUSED int n = snprintf(buf, PATH_MAX, "%s/lib/%s", c->sysroot, filename);
  safecheck(n < PATH_MAX);
  return buf;
}


static bool must_build_libc(compiler_t* c) {
  char buf[PATH_MAX];
  const char* filename;
  switch ((enum target_sys)c->target.sys) {
    case SYS_macos: filename = "libSystem.tbd"; break;
    case SYS_linux: filename = "libc.a"; break;
    case SYS_none: case SYS_COUNT: safefail("invalid target"); break;
  };
  dlog("check %s", relpath(lib_path(c, buf, filename)));
  return !fs_isfile(lib_path(c, buf, filename));
}


static err_t build_libc(compiler_t* c) {
  dlog("%s", __FUNCTION__);
  if (c->target.sys == SYS_macos) {
    // copy .tbd files (of which most are symlinks)
    dlog("TODO: copy .tbd files (of which most are symlinks)");
  }
  return 0;
}


static bool must_build_librt(compiler_t* c) {
  char buf[PATH_MAX];
  dlog("check %s", relpath(lib_path(c, buf, "librt.a")));
  return !fs_isfile(lib_path(c, buf, "librt.a"));
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
        cobj_setobjfilef(b, obj, "aarch64/lse_%s_%u_%u.o", pat, 1u << sizem, model);
      }
    }
  }
  return 0;
}


static err_t build_librt(compiler_t* c) {
  dlog("%s", __FUNCTION__);

  cbuild_t build;
  cbuild_init(&build, c, "librt");
  build.srcdir = path_join_alloca(coroot, "lib/librt");

  // see compiler-rt/lib/builtins/CMakeLists.txt
  strlist_add(&build.cc,
    "-std=c11",
    "-Os",
    "-fPIC",
    "-fno-builtin",
    "-fomit-frame-pointer",
    "-fvisibility=hidden");

  if (c->target.arch == ARCH_riscv32)
    strlist_add(&build.cc, "-fforce-enable-int128");

  strlist_addf(&build.cc, "-I%s", build.srcdir);

  // TODO: cmake COMPILER_RT_HAS_FCF_PROTECTION_FLAG
  // if (compiler_accepts_flag_for_target("-fcf-protection=full"))
  //   strlist_add(&build.cc, "-fcf-protection=full")

  // TODO: cmake COMPILER_RT_HAS_ASM_LSE
  // if (compiles("asm(\".arch armv8-a+lse\");asm(\"cas w0, w1, [x2]\");"))
  //   strlist_add(&build.cc, "-DHAS_ASM_LSE=1");

  // TODO: cmake COMPILER_RT_HAS_FLOAT16
  // if (compiles("_Float16 f(_Float16 x){return x;}"))
  //   strlist_add(&build.cc, "-DCOMPILER_RT_HAS_FLOAT16=1");

  strlist_add(&build.cc, c->buildmode == BUILDMODE_OPT ? "-flto=thin" : "-g");
  err_t err = 0;

  // find source list for target
  const librt_srclist_t* srclist = NULL;
  for (u32 i = 0; i < countof(librt_srclist); i++) {
    const librt_srclist_t* t = &librt_srclist[i];
    if (c->target.arch == t->arch && c->target.sys == t->sys &&
        strcmp(c->target.sysver, t->sysver) == 0)
    {
      srclist = t;
      break;
    }
  }
  if (!srclist)
    panic("no librt impl for target");

  // find sources
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

  // TODO: cmake COMPILER_RT_HAS_BFLOAT16
  // if (compiles("__bf16 f(__bf16 x) {return x;}")) {
  //   strlist_addf(&srclist, "%s/truncdfbf2.c.o", objdir);
  //   strlist_add(&srclist, "truncdfbf2.c");
  //   strlist_addf(&srclist, "%s/truncsfbf2.c.o", objdir);
  //   strlist_add(&srclist, "truncsfbf2.c");
  // }

  char tmpbuf[PATH_MAX];
  const char* outfile = lib_path(c, tmpbuf, "librt.a");
  err = cbuild_build(&build, outfile);

end:
  cbuild_dispose(&build);
  return err;
}


err_t build_syslibs_if_needed(compiler_t* c) {
  err_t err = 0;
  if (must_build_libc(c) && (err = build_libc(c)))
    return err;
  if (must_build_librt(c) && (err = build_librt(c)))
    return err;
  return err;
}
