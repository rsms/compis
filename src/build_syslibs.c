// SPDX-License-Identifier: Apache-2.0
#include "colib.h"
#include "compiler.h"
#include "cbuild.h"
#include "strlist.h"
#include "path.h"
#include "bgtask.h"
#include "lockfile.h"
#include "llvm/llvm.h"

#include "librt_info.h"
#include "musl_info.h"

#include <dirent.h>
#include <err.h>
#include <errno.h>
#include <libgen.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>


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


static err_t copy_files(const char* srcdir, const char* dstdir) {
  dlog("copy_files %s/ -> %s/", relpath(srcdir), relpath(dstdir));

  DIR* dp = opendir(srcdir);
  if (dp == NULL) {
    warn("opendir(%s)", srcdir);
    return err_errno();
  }

  char srcfile[PATH_MAX];
  char dstfile[PATH_MAX];

  usize srcfilelen = MIN(strlen(srcdir), PATH_MAX - 2);
  memcpy(srcfile, srcdir, srcfilelen);
  srcfile[srcfilelen++] = '/';

  usize dstfilelen = MIN(strlen(dstdir), PATH_MAX - 2);
  memcpy(dstfile, dstdir, dstfilelen);
  dstfile[dstfilelen++] = '/';

  err_t err = 0;
  struct dirent* d;

  while ((d = readdir(dp)) != NULL) {
    if (*d->d_name == '.')
      continue;

    usize srcnamelen = MIN(strlen(d->d_name), PATH_MAX - srcfilelen);
    memcpy(&srcfile[srcfilelen], d->d_name, srcnamelen);
    srcfile[srcfilelen + srcnamelen] = 0;

    usize dstnamelen = MIN(strlen(d->d_name), PATH_MAX - dstfilelen);
    memcpy(&dstfile[dstfilelen], d->d_name, dstnamelen);
    dstfile[dstfilelen + dstnamelen] = 0;

    dlog("cp %s -> %s", relpath(srcfile), relpath(dstfile));
    if (( err = fs_copyfile(srcfile, dstfile, 0) ))
      break;
  }
  closedir(dp);
  return err;
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


static err_t copy_sysroot_lib_dir(const char* path, void* cp) {
  compiler_t* c = cp;
  if (!fs_isdir(path))
    return 0;
  char* dstdir = path_join_alloca(c->sysroot, "lib");
  return copy_files(path, dstdir);
}


static err_t build_libc_musl(compiler_t* c) {
  char tmpbuf[PATH_MAX];

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
                          "-Isrc/internal" );
  strlist_addf(&build.cc, "-Iinclude/%s", arch_name(c->target.arch));
  strlist_add(&build.cc,  "-Iinclude" );
  strlist_add_array(&build.cc, c->cflags_sysinc.strings, c->cflags_sysinc.len);

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
  }
  ADD_CRT_SOURCE(crt1, "-DCRT")
  ADD_CRT_SOURCE(rcrt1, "-DCRT", "-fPIC")
  ADD_CRT_SOURCE(Scrt1, "-DCRT", "-fPIC")
  ADD_CRT_SOURCE(crti, "-DCRT")
  ADD_CRT_SOURCE(crtn, "-DCRT")
  #undef ADD_CRT_SOURCE

  // build
  const char* outfile = lib_path(c, tmpbuf, "libc.a");
  err = cbuild_build(&build, outfile);

end:
  cbuild_dispose(&build);
  return err;
}


static err_t build_libc(compiler_t* c) {
  switch ((enum target_sys)c->target.sys) {
    case SYS_macos:
      return target_visit_dirs(&c->target, "darwin", copy_sysroot_lib_dir, c);
    case SYS_linux:
      return build_libc_musl(c);
    case SYS_COUNT:
    case SYS_none:
      break;
  }
  safefail("target.sys #%u", c->target.sys);
  return ErrInvalid;
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
        cobj_setobjfilef(b, obj, "aarch64.lse_%s_%u_%u.o", pat, 1u << sizem, model);
      }
    }
  }
  return 0;
}


static err_t build_librt(compiler_t* c) {
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
  const librt_srclist_t* srclist = FIND_SRCLIST(&c->target, librt_srclist);

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


static err_t create_ld_symlinks(compiler_t* c) {
  usize dstpathcap = strlen(coexefile) + 16;
  char* dstpath = alloca(dstpathcap);
  usize dstpathlen = path_dir(dstpath, dstpathcap, coexefile);
  const char* coexename = path_base(coexefile);
  int r;

  #define CREATE_EXE_SYMLINK(name) { \
    memcpy(&dstpath[dstpathlen], \
      PATH_SEPARATOR_STR name, strlen(PATH_SEPARATOR_STR name) + 1); \
    r = symlink(coexename, dstpath); \
    if (r != 0 && errno != EEXIST) \
      return err_errno(); \
    if (r == 0) dlog("symlink %s -> %s", dstpath, coexename); \
  }

  CREATE_EXE_SYMLINK("ld.lld");
  CREATE_EXE_SYMLINK("ld64.lld");
  CREATE_EXE_SYMLINK("wasm-ld");
  CREATE_EXE_SYMLINK("lld-link");

  #undef CREATE_EXE_SYMLINK
  return 0;
}


err_t build_syslibs_if_needed(compiler_t* c) {
  err_t err = 0;

  if (must_build_libc(c) || must_build_librt(c)) {
    lockfile_t lockfile;
    char* lockfile_path = path_join_alloca(c->sysroot, "syslibs-build.lock");
    long lockee_pid;
    if (( err = lockfile_trylock(&lockfile, lockfile_path, &lockee_pid) )) {
      if (err != ErrExists)
        return err;
      log("waiting for compis (pid %ld) to finish...", lockee_pid);
      if (( err = lockfile_lock(&lockfile, lockfile_path) ))
        return err;
    }
    // note: must check again in case another process won and build the libs
    if (!err && must_build_libc(c)) err = build_libc(c);
    if (!err && must_build_librt(c)) err = build_librt(c);
    lockfile_unlock(&lockfile);
  }

  if (!err)
    err = create_ld_symlinks(c);

  return err;
}
