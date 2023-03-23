#include "colib.h"
#include "cbuild.h"
#include "subproc.h"
#include "path.h"
#include "llvm/llvm.h"

#include <unistd.h>
#include <libgen.h>
#include <sys/stat.h>
#include <stdlib.h>
#include <string.h>


#define OBJ_DIR_PREFIX "tmp-"


void cbuild_init(cbuild_t* b, compiler_t* c, const char* name, const char* builddir) {
  b->c = c;
  b->cc = strlist_make(c->ma, "cc");
  b->cxx = strlist_make(c->ma, "c++");
  b->as = strlist_make(c->ma, "cc");
  strlist_add_array(&b->cc, c->cflags_common.strings, c->cflags_common.len);
  strlist_add_array(&b->cxx, c->cflags_common.strings, c->cflags_common.len);
  strlist_add_array(&b->as, c->flags_common.strings, c->flags_common.len);

  // "{builddir}/{tmp}/{name}"
  usize namelen = strlen(name);
  usize cap = strlen(builddir) + 1 + strlen(OBJ_DIR_PREFIX) + namelen + 1;
  b->objdir = mem_alloc(c->ma, cap).p;
  if (!b->objdir) {
    b->cc.ok = false;
  } else {
    snprintf(b->objdir, cap, "%s" PATH_SEP_STR OBJ_DIR_PREFIX "%s", builddir, name);
    // name is at end of b->objdir
    b->name = b->objdir + (cap - namelen - 1);
  }

  b->srcdir = ".";
  memset(&b->cc_snapshot, 0, sizeof(b->cc_snapshot));
  memset(&b->cxx_snapshot, 0, sizeof(b->cxx_snapshot));
  memset(&b->as_snapshot, 0, sizeof(b->as_snapshot));
  memset(&b->objs, 0, sizeof(b->objs));
}


void cbuild_dispose(cbuild_t* b) {
  memalloc_t ma = b->c->ma;
  for (u32 i = 0; i < b->objs.len; i++) {
    cobj_t* obj = &b->objs.v[i];
    mem_freecstr(ma, obj->objfile);
    if (obj->cflags && !obj->cflags_external)
      strlist_dispose(obj->cflags);
  }
  cobjarray_dispose(&b->objs, ma);
  mem_freecstr(ma, b->objdir);
  strlist_dispose(&b->cc);
  strlist_dispose(&b->cxx);
  strlist_dispose(&b->as);
}


static cobj_srctype_t detect_srctype(const char* filename) {
  const char* ext = path_ext(filename);
  if (strieq(ext, ".c"))                         return COBJ_TYPE_C;
  if (strieq(ext, ".cc") || strieq(ext, ".cpp")) return COBJ_TYPE_CXX;
  if (strieq(ext, ".s"))                         return COBJ_TYPE_ASSEMBLY;
  panic("unknown file extension: \"%s\" (%s)", ext, filename);
  return 0;
}


cobj_t* nullable cbuild_add_source(cbuild_t* b, const char* srcfile) {
  assertf(!cbuild_config_ended(b), "%s called after cbuild_end_config", __FUNCTION__);
  if UNLIKELY(b->objs.len >= b->objs.cap && !cobjarray_reserve(&b->objs, b->c->ma, 1)) {
    b->cc.ok = false;
    return NULL;
  }
  cobj_t* obj = &b->objs.v[b->objs.len++];
  memset(obj, 0, sizeof(*obj));
  obj->srcfile = srcfile;
  obj->srctype = detect_srctype(srcfile);
  return obj;
}


strlist_t* nullable cobj_cflags(cbuild_t* b, cobj_t* obj) {
  if (!obj->cflags) {
    if (!( obj->cflags = mem_alloct(b->c->ma, strlist_t) ))
      return NULL;
    strlist_init(obj->cflags, b->c->ma);
  }
  return obj->cflags;
}


bool cobj_addcflagf(cbuild_t* b, cobj_t* obj, const char* fmt, ...) {
  if (!cobj_cflags(b, obj))
    return false;
  va_list ap;
  va_start(ap, fmt);
  strlist_addv(obj->cflags, fmt, ap);
  va_end(ap);
  return true;
}


ATTR_FORMAT(printf, 3, 4)
void cobj_setobjfilef(cbuild_t* b, cobj_t* obj, const char* fmt, ...) {
  char tmpbuf[PATH_MAX];

  va_list ap;
  va_start(ap, fmt);
  int len = vsnprintf(tmpbuf, sizeof(tmpbuf), fmt, ap);
  va_end(ap);

  if (len < 0 || len >= (int)sizeof(tmpbuf)) {
    dlog("%s overflow", __FUNCTION__);
    b->cc.ok = false;
  }

  mem_freecstr(b->c->ma, obj->objfile);

  if (*tmpbuf == 0) {
    obj->objfile = NULL;
    return;
  }

  obj->objfile = mem_alloctv(b->c->ma, char, (usize)len + 1);
  if (!obj->objfile) {
    b->cc.ok = false;
  } else {
    memcpy(obj->objfile, tmpbuf, (usize)len + 1);
  }
}


void cbuild_end_config(cbuild_t* b) {
  assertf(!cbuild_config_ended(b), "%s called twice", __FUNCTION__);
  strlist_add(&b->cc, "-c", "-o");
  strlist_add(&b->cxx, "-c", "-o");
  strlist_add(&b->as, "-c", "-o");
  b->cc_snapshot = strlist_save(&b->cc);
  b->cxx_snapshot = strlist_save(&b->cxx);
  b->as_snapshot = strlist_save(&b->as);
}


static const char* cbuild_objfile(cbuild_t* b, const cobj_t* obj) {
  if (obj->objfile && *obj->objfile) {
    if (path_isabs(obj->objfile))
      return obj->objfile;
    snprintf(b->objfile, sizeof(b->objfile), "%s/%s", b->objdir, obj->objfile);
    return b->objfile;
  }
  // based on srcfile, e.g. "foo/bar.c" => "{objdir}/foo bar.c.o"
  int len = snprintf(b->objfile, sizeof(b->objfile), "%s/%s.o", b->objdir, obj->srcfile);
  safecheckf((usize)len < sizeof(b->objfile), "pathname overflow %s", obj->srcfile);
  // sub '/' -> ' '
  for (usize i = strlen(b->objdir) + 1; i < (usize)len; i++) {
    if (b->objfile[i] == '/')
      b->objfile[i] = ' ';
  }
  return b->objfile;
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


static err_t cbuild_mkdirs(cbuild_t* b) {
  err_t err = 0;
  char dir[PATH_MAX];
  ptrarray_t dirs = {0};

  for (u32 i = 0; i < b->objs.len; i++) {
    cobj_t* obj = &b->objs.v[i];
    if (path_dir_buf(dir, sizeof(dir), cbuild_objfile(b, obj)) >= sizeof(dir)) {
      err = ErrOverflow;
      break;
    }
    if UNLIKELY(!array_sortedset_addcstr(&dirs, b->c->ma, dir)) {
      err = ErrNoMem;
      break;
    }
  }

  if (!err) for (u32 i = 0; i < dirs.len; i++) {
    const char* dir = (const char*)dirs.v[i];
    if (( err = fs_mkdirs(dir, 0755, 0) ))
      break;
  }

  ptrarray_dispose(&dirs, b->c->ma);
  return err;
}


static err_t cbuild_clean_objdir(cbuild_t* b) {
  err_t err = fs_remove(b->objdir);
  if (err && err != ErrNotFound)
    elog("cbuild_clean_objdir \"%s\": %s", b->objdir, err_str(err));
  return err;
}


static err_t cbuild_build_compile(cbuild_t* b, bgtask_t* task, strlist_t* objfiles) {
  // create subprocs attached to promise
  promise_t promise = {0};
  subprocs_t* subprocs = subprocs_create_promise(b->c->ma, &promise);
  if (!subprocs)
    return ErrNoMem;

  err_t err = 0;

  for (u32 i = 0; i < b->objs.len; i++) {
    cobj_t* obj = &b->objs.v[i];
    const char* objfile = cbuild_objfile(b, obj);
    strlist_t* args = NULL;
    strlist_t snapshot = {0};
    switch ((enum cobj_srctype)obj->srctype) {
      case COBJ_TYPE_C:        snapshot = b->cc_snapshot;  args = &b->cc; break;
      case COBJ_TYPE_CXX:      snapshot = b->cxx_snapshot; args = &b->cxx; break;
      case COBJ_TYPE_ASSEMBLY: snapshot = b->as_snapshot;  args = &b->as; break;
    }

    strlist_add(args, objfile, obj->srcfile);
    if (obj->cflags)
      strlist_add_list(args, obj->cflags);

    task->n++;
    if (obj->objfile && *obj->objfile) {
      // custom objfile (ie may be compiling the same source as multiple objects)
      bgtask_setstatusf(task, "compile %s (%s)",
        relpath(obj->srcfile), path_base(obj->objfile));
    } else {
      bgtask_setstatusf(task, "compile %s", relpath(obj->srcfile));
    }

    err = compiler_spawn_tool(b->c, subprocs, args, b->srcdir);
    strlist_restore(args, snapshot);
    if (err)
      break;

    if ((obj->flags & COBJ_EXCLUDE_FROM_LIB) == 0)
      strlist_add(objfiles, objfile);
  }

  // wait for compiler jobs to complete
  if (err)
    subprocs_cancel(subprocs);
  err_t err1 = promise_await(&promise);
  return err == 0 ? err1 : err;
}


static err_t cbuild_create_archive(
  cbuild_t* b, bgtask_t* task, const char* outfile, char*const* objv, u32 objc)
{
  task->n++;
  bgtask_setstatusf(task, "create %s from %u objects", relpath(outfile), objc);

  err_t err;

  char* dir = path_dir_alloca(outfile);
  if (( err = fs_mkdirs(dir, 0755, FS_VERBOSE) ))
    return err;

  CoLLVMArchiveKind arkind;
  if (b->c->target.sys == SYS_none) {
    arkind = llvm_sys_archive_kind(target_default()->sys);
  } else {
    arkind = llvm_sys_archive_kind(b->c->target.sys);
  }

  char* errmsg = "?";
  err = llvm_write_archive(arkind, outfile, objv, objc, &errmsg);
  if (!err)
    return 0;
  elog("llvm_write_archive: (err=%s) %s", err_str(err), errmsg);
  if (err == ErrNotFound) {
    for (u32 i = 0; i < objc; i++) {
      if (!fs_isfile(objv[i]))
        elog("%s: file not found", objv[i]);
    }
  }
  LLVMDisposeMessage(errmsg);
  return err;
}


u32 cbuild_njobs(const cbuild_t* b) {
  u32 npost_jobs = 1; // ar
  return b->objs.len + npost_jobs;
}


err_t cbuild_build(cbuild_t* b, const char* outfile, bgtask_t* nullable usertask) {
  if (!cbuild_config_ended(b))
    cbuild_end_config(b);

  if (b->objs.len == 0) {
    dlog("cbuild has no sources");
    return ErrEnd;
  }

  // check for memory allocation failures
  if (!cbuild_ok(b)) {
    dlog("cbuild_ok");
    return ErrNoMem;
  }
  for (u32 i = 0; i < b->objs.len; i++) {
    if (b->objs.v[i].cflags && !b->objs.v[i].cflags->ok) {
      dlog("obj(%s).cflags.ok==false", b->objs.v[i].srcfile);
      return ErrNoMem;
    }
  }

  // create directories
  err_t err = cbuild_mkdirs(b);
  if (err) {
    dlog("cbuild_mkdirs");
    return err;
  }

  // ar invocation which we add objfiles to
  // strlist_t ar = strlist_make(b->c->ma, "ar", "rcs", outfile);
  strlist_t objfiles = strlist_make(b->c->ma);

  // create task, unless provided by caller
  bgtask_t* task = usertask;
  if (!usertask)
    task = bgtask_start(b->c->ma, b->name, cbuild_njobs(b), 0);

  // compile objects
  err = cbuild_build_compile(b, task, &objfiles);
  if (err)
    goto end;

  // archive objects
  char* const* objfilev = strlist_array(&objfiles);
  if (!objfiles.ok) {
    dlog("strlist_array failed");
    err = ErrNoMem;
  } else {
    err = cbuild_create_archive(b, task, outfile, objfilev, objfiles.len);
  }

end:
  cbuild_clean_objdir(b);
  if (!usertask)
    bgtask_end(task, "");
  strlist_dispose(&objfiles);
  return err;
}
