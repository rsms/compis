// SPDX-License-Identifier: Apache-2.0
#include "colib.h"
#include "compiler.h"
#include "subproc.h"
#include "strlist.h"
#include "path.h"
#include "llvm/llvm.h"

#include <unistd.h>
#include <libgen.h>
#include <sys/stat.h>


static const char* librt_sources[] = {
  // (cd deps/llvmbox/src/builtins && ls -1 *.c)
  "absvdi2.c",
  "absvsi2.c",
  "absvti2.c",
  "adddf3.c",
  "addsf3.c",
  "addtf3.c",
  "addvdi3.c",
  "addvsi3.c",
  "addvti3.c",
  "apple_versioning.c",
  "ashldi3.c",
  "ashlti3.c",
  "ashrdi3.c",
  "ashrti3.c",
  "bswapdi2.c",
  "bswapsi2.c",
  "clear_cache.c",
  "clzdi2.c",
  "clzsi2.c",
  "clzti2.c",
  "cmpdi2.c",
  "cmpti2.c",
  "comparedf2.c",
  "comparesf2.c",
  "comparetf2.c",
  "ctzdi2.c",
  "ctzsi2.c",
  "ctzti2.c",
  "divdc3.c",
  "divdf3.c",
  "divdi3.c",
  "divmoddi4.c",
  "divmodsi4.c",
  "divmodti4.c",
  "divsc3.c",
  "divsf3.c",
  "divsi3.c",
  "divtc3.c",
  "divtf3.c",
  "divti3.c",
  "extenddftf2.c",
  "extendhfsf2.c",
  "extendhftf2.c",
  "extendsfdf2.c",
  "extendsftf2.c",
  "ffsdi2.c",
  "ffssi2.c",
  "ffsti2.c",
  "fixdfdi.c",
  "fixdfsi.c",
  "fixdfti.c",
  "fixsfdi.c",
  "fixsfsi.c",
  "fixsfti.c",
  "fixtfdi.c",
  "fixtfsi.c",
  "fixtfti.c",
  "fixunsdfdi.c",
  "fixunsdfsi.c",
  "fixunsdfti.c",
  "fixunssfdi.c",
  "fixunssfsi.c",
  "fixunssfti.c",
  "fixunstfdi.c",
  "fixunstfsi.c",
  "fixunstfti.c",
  "floatdidf.c",
  "floatdisf.c",
  "floatditf.c",
  "floatsidf.c",
  "floatsisf.c",
  "floatsitf.c",
  "floattidf.c",
  "floattisf.c",
  "floattitf.c",
  "floatundidf.c",
  "floatundisf.c",
  "floatunditf.c",
  "floatunsidf.c",
  "floatunsisf.c",
  "floatunsitf.c",
  "floatuntidf.c",
  "floatuntisf.c",
  "floatuntitf.c",
  "fp_mode.c",
  "int_util.c",
  "lshrdi3.c",
  "lshrti3.c",
  "moddi3.c",
  "modsi3.c",
  "modti3.c",
  "muldc3.c",
  "muldf3.c",
  "muldi3.c",
  "mulodi4.c",
  "mulosi4.c",
  "muloti4.c",
  "mulsc3.c",
  "mulsf3.c",
  "multc3.c",
  "multf3.c",
  "multi3.c",
  "mulvdi3.c",
  "mulvsi3.c",
  "mulvti3.c",
  "negdf2.c",
  "negdi2.c",
  "negsf2.c",
  "negti2.c",
  "negvdi2.c",
  "negvsi2.c",
  "negvti2.c",
  "os_version_check.c",
  "paritydi2.c",
  "paritysi2.c",
  "parityti2.c",
  "popcountdi2.c",
  "popcountsi2.c",
  "popcountti2.c",
  "powidf2.c",
  "powisf2.c",
  "powitf2.c",
  "subdf3.c",
  "subsf3.c",
  "subtf3.c",
  "subvdi3.c",
  "subvsi3.c",
  "subvti3.c",
  "trampoline_setup.c",
  "truncdfhf2.c",
  "truncdfsf2.c",
  "truncsfhf2.c",
  "trunctfdf2.c",
  "trunctfhf2.c",
  "trunctfsf2.c",
  "ucmpdi2.c",
  "ucmpti2.c",
  "udivdi3.c",
  "udivmoddi4.c",
  "udivmodsi4.c",
  "udivmodti4.c",
  "udivsi3.c",
  "udivti3.c",
  "umoddi3.c",
  "umodsi3.c",
  "umodti3.c",
};


static bool file_exists(const char* path) {
  struct stat st;
  if (stat(path, &st) != 0)
    return false;
  return S_ISREG(st.st_mode);
}


static char* lib_path(compiler_t* c, char buf[PATH_MAX], const char* filename) {
  UNUSED int n = snprintf(buf, PATH_MAX, "%s/lib/%s", c->sysroot, filename);
  safecheck(n < PATH_MAX);
  return buf;
}


static bool must_build_libc(compiler_t* c) {
  char buf[PATH_MAX];
  const char* filename = c->target.sys == SYS_macos ? "libSystem.tbd" : "libc.a";
  dlog("check %s", lib_path(c, buf, filename));
  return !file_exists(lib_path(c, buf, filename));
}

static err_t build_libc(compiler_t* c, subprocs_t* subprocs) {
  dlog("%s", __FUNCTION__);
  if (c->target.sys == SYS_macos) {
    // copy .tbd files (of which most are symlinks)
    dlog("TODO: copy .tbd files (of which most are symlinks)");
  }
  return 0;
}


static const char* sys_getcwd() {
  static char buf[PATH_MAX];
  return getcwd(buf, sizeof(buf));
}


static void srclist_add(
  strlist_t* srclist, const char* objdir, const char** srcv, usize srcc)
{
  for (usize i = 0; i < srcc; i++) {
    strlist_addf(srclist, "%s/%s.o", objdir, srcv[i]);
    strlist_add(srclist, srcv[i]);
  }
}


static char* const* nullable srclist_finalize(strlist_t* srclist) {
  char* const* v = strlist_array(srclist);
  return srclist->ok ? v : NULL;
}


static int str_cmp(const char** a, const char** b, void* ctx) {
  return strcmp(*a, *b);
}


static bool array_sortedset_addcstr(ptrarray_t* a, memalloc_t ma, const char* str) {
  const char** vp = array_sortedset_assign(
    const char*, a, ma, &str, (array_sorted_cmp_t)str_cmp, NULL);
  if UNLIKELY(!vp)
    return false;
  if (*vp)
    return true;
  return (*vp = mem_strdup(ma, slice_cstr(str), 0)) != NULL;
}


static err_t srclist_mkdirs(strlist_t* srclist) {
  err_t err = 0;
  char dir[PATH_MAX];
  memalloc_t ma = srclist->buf.ma;
  ptrarray_t dirs = {0};

  char* const* v = strlist_array(srclist);
  for (int i = 0; i < srclist->len; i += 2) {
    if (dirname_r(v[i], dir) == NULL) {
      err = ErrOverflow;
      break;
    }
    if UNLIKELY(!array_sortedset_addcstr(&dirs, ma, dir)) {
      err = ErrNoMem;
      break;
    }
  }

  if (!err) for (u32 i = 0; i < dirs.len; i++) {
    const char* dir = (const char*)dirs.v[i];
    if (( err = fs_mkdirs(dir, strlen(dir), 0755) ))
      break;
  }

  ptrarray_dispose(&dirs, ma);
  return err;
}


static bool must_build_librt(compiler_t* c) {
  char buf[PATH_MAX];
  dlog("check %s", lib_path(c, buf, "librt.a"));
  return !file_exists(lib_path(c, buf, "librt.a"));
}


static err_t build_librt(compiler_t* c, subprocs_t* subprocs) {
  dlog("%s", __FUNCTION__);

  strlist_t cc = strlist_make(c->ma, "cc");
  strlist_add_array(&cc, c->cflags_common.strings, c->cflags_common.len);
  strlist_add(&cc,
    "-std=c11",
    "-Os",
    "-fPIC",
    "-fno-builtin",
    "-fomit-frame-pointer",
    c->buildmode == BUILDMODE_OPT ? "-flto=thin" : "-g",
    "-c", "-o");

  strlist_t cc_snapshot = strlist_save(&cc);
  err_t err = 0;
  char* srcdir = path_join_alloca(coroot, "deps/llvmbox/src/builtins");
  char* objdir = path_join_alloca(sys_getcwd(), c->builddir, "librt.obj");

  // build list of source & output file pairs
  strlist_t srclist = strlist_make(c->ma);
  srclist_add(&srclist, objdir, librt_sources, countof(librt_sources));
  char* const* srclistv = srclist_finalize(&srclist);
  if (!srclistv) {
    err = ErrNoMem;
    goto end;
  }
  if (( err = srclist_mkdirs(&srclist) ))
    goto end;

  // spawn compilation processes
  for (int i = 0; i < srclist.len; i += 2) {
    log("cc %s -> %s", srclistv[i+1], srclistv[i]);
    strlist_add(&cc, srclistv[i], srclistv[i+1]);

    u64 t = nanotime();

    err = compiler_spawn_tool(c, subprocs, &cc, srcdir);

    char duration[25];
    fmtduration(duration, nanotime() - t);
    dlog("compiler_spawn_tool finished in %s", duration);

    // undo addition of source-specific args to cc
    strlist_restore(&cc, cc_snapshot);

    if (err)
      break;
  }

end:
  strlist_dispose(&cc);
  return err;
}


err_t build_syslibs_if_needed(compiler_t* c) {
  bool need_libc = must_build_libc(c);
  bool need_librt = must_build_librt(c);

  if (!(need_libc | need_librt))
    return 0;

  promise_t promise = {0};

  // create subprocs attached to promise
  subprocs_t* subprocs = subprocs_create_promise(c->ma, &promise);
  if (!subprocs)
    return ErrNoMem;

  err_t err = 0;
  if (need_libc)
    err = build_libc(c, subprocs);
  if (need_librt && !err)
    err = build_librt(c, subprocs);

  if (err) {
    subprocs_cancel(subprocs);
    return err;
  }

  return promise_await(&promise);
}
