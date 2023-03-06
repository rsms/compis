#include "colib.h"
#include "cbuild.h"
#include "subproc.h"
#include "path.h"
#include "bgtask.h"
#include "llvm/llvm.h"

#include <unistd.h>
#include <libgen.h>
#include <sys/stat.h>
#include <stdlib.h>
#include <string.h>


#define OBJDIR_SUFFIX ".obj"


void cbuild_init(cbuild_t* b, compiler_t* c, const char* name) {
  b->c = c;
  b->cc = strlist_make(c->ma, "cc");
  b->as = strlist_make(c->ma, "cc");
  strlist_add_array(&b->cc, c->cflags_common.strings, c->cflags_common.len);
  strlist_add_array(&b->as, c->sflags_common.strings, c->sflags_common.len);

  usize namelen = strlen(name);
  usize objdircap = strlen(c->builddir) + 1 + namelen + strlen(OBJDIR_SUFFIX) + 1;
  b->objdir = mem_alloctv(c->ma, char, objdircap + namelen + 1);
  if (!b->objdir) {
    b->cc.ok = false;
  } else {
    snprintf(b->objdir, objdircap, "%s/%s%s", c->builddir, name, OBJDIR_SUFFIX);
    b->name = b->objdir + objdircap;
    memcpy(b->name, name, namelen + 1);
  }

  b->srcdir = ".";
  memset(&b->cc_snapshot, 0, sizeof(b->cc_snapshot));
  memset(&b->as_snapshot, 0, sizeof(b->cc_snapshot));
  memset(&b->objs, 0, sizeof(b->objs));
}


void cbuild_dispose(cbuild_t* b) {
  memalloc_t ma = b->c->ma;
  for (u32 i = 0; i < b->objs.len; i++) {
    cobj_t* obj = &b->objs.v[i];
    mem_freecstr(ma, obj->objfile);
    if (obj->cflags)
      strlist_dispose(obj->cflags);
  }
  cobjarray_dispose(&b->objs, ma);
  mem_freecstr(ma, b->objdir);
  strlist_dispose(&b->cc);
  strlist_dispose(&b->as);
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
  if (strcasecmp(path_ext(srcfile), ".s") == 0)
    obj->srctype = COBJ_TYPE_ASSEMBLY;
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
  strlist_add(&b->as, "-c", "-o");
  b->cc_snapshot = strlist_save(&b->cc);
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
  // memalloc_t ma, const char* objdir, const cobj_t* objs, usize len
  err_t err = 0;
  char dir[PATH_MAX];
  ptrarray_t dirs = {0};

  for (u32 i = 0; i < b->objs.len; i++) {
    cobj_t* obj = &b->objs.v[i];
    if (path_dir(dir, sizeof(dir), cbuild_objfile(b, obj)) >= sizeof(dir)) {
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
    if (( err = fs_mkdirs(dir, 0755) ))
      break;
  }

  ptrarray_dispose(&dirs, b->c->ma);
  return err;
}


static err_t cbuild_clean_objdir(cbuild_t* b) {
  err_t err = fs_remove(b->objdir);
  if (err && err != ErrNotFound)
    log("cbuild_clean_objdir \"%s\": %s", b->objdir, err_str(err));
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
    strlist_t* args = obj->srctype == COBJ_TYPE_C ? &b->cc : &b->as;

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
    strlist_restore(args, obj->srctype == COBJ_TYPE_C ? b->cc_snapshot : b->as_snapshot);
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
  if (( err = fs_mkdirs(dir, 0755) ))
    return err;

  CoLLVMArchiveKind arkind = llvm_sys_archive_kind(b->c->target.sys);
  char* errmsg = "?";
  err = llvm_write_archive(arkind, outfile, objv, objc, &errmsg);
  if (!err)
    return 0;
  log("llvm_write_archive: (err=%s) %s", err_str(err), errmsg);
  if (err == ErrNotFound) {
    for (u32 i = 0; i < objc; i++) {
      if (!fs_isfile(objv[i]))
        log("%s: file not found", objv[i]);
    }
  }
  LLVMDisposeMessage(errmsg);
  return err;
}


static err_t cbuild_build(cbuild_t* b, const char* outfile) {
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

  // create task
  u32 npost_jobs = 1; // ar
  bgtask_t* task = bgtask_start(b->c->ma, b->name, b->objs.len + npost_jobs, 0);

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
  bgtask_end(task, "");
  strlist_dispose(&objfiles);
  return err;
}
