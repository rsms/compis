// SPDX-License-Identifier: Apache-2.0
#include "colib.h"
#include "compiler.h"
#include "path.h"
#include "dirwalk.h"

#include <sys/stat.h>
#include <err.h>


static int srcfile_cmp(const srcfile_t* a, const srcfile_t* b, void* ctx) {
  return strcmp(a->name.p, b->name.p);
}


err_t pkg_init(pkg_t* pkg, memalloc_t ma) {
  // *pkg is assumed to be zeroed
  assertf(memcmp(pkg, &(pkg_t){0}, sizeof(*pkg)) == 0, "pkg not zeroed");
  err_t err = rwmutex_init(&pkg->defs_mu);
  if (err)
    return err;
  if (( err = map_init(&pkg->defs, ma, 32) ? 0 : ErrNoMem ))
    goto end_err1;
  if (( err = typefuntab_init(&pkg->tfundefs, ma) ))
    goto end_err2;
  return 0;

end_err2:
  map_dispose(&pkg->defs, ma);
end_err1:
  rwmutex_dispose(&pkg->defs_mu);
  return err;
}


void pkg_dispose(pkg_t* pkg, memalloc_t ma) {
  str_free(pkg->path);
  str_free(pkg->dir);
  if (pkg->defs.cap == 0)
    return;
  array_dispose(srcfile_t, (array_t*)&pkg->files, ma);
  ptrarray_dispose(&pkg->imports, ma);
  map_dispose(&pkg->defs, ma);
  rwmutex_dispose(&pkg->defs_mu);
  typefuntab_dispose(&pkg->tfundefs, ma);
}


srcfile_t* pkg_add_srcfile(pkg_t* pkg, const char* name) {
  // add to sorted set, or retrieve existing with same name
  srcfile_t tmp = { .name = str_make(name) };
  srcfile_t* f = array_sortedset_assign(
    srcfile_t, (array_t*)&pkg->files, memalloc_ctx(),
    &tmp, (array_sorted_cmp_t)srcfile_cmp, NULL);
  safecheckf(f, "out of memory");

  if (f->name.p != NULL) {
    // duplicate
    // dlog("%s: skip duplicate %s", __FUNCTION__, name);
    str_free(tmp.name);
    return f;
  }

  // note: array_sortedset_assign returns new entries zero initialized

  // assign ids
  for (u32 i = 0; i < pkg->files.len; i++)
    pkg->files.v[i].id = i;

  // dlog("%s: add %s", __FUNCTION__, name);
  f->pkg = pkg;
  f->name = tmp.name;
  f->type = filetype_guess(f->name.p);
  return f;
}


err_t pkg_find_files(pkg_t* pkg) {
  if (pkg->dir.len == 0)
    return ErrNotFound;

  memalloc_t ma = memalloc_ctx();
  dirwalk_t* dw = dirwalk_open(ma, pkg->dir.p, 0);
  if (!dw)
    return ErrNoMem;

  if (pkg->files.len > 0) {
    dlog("TODO: free exising `srcfile_t`s");
    return ErrExists;
  }

  err_t err = 0;

  while ((err = dirwalk_next(dw)) > 0) {
    if (dw->type != S_IFREG) // ignore
      continue;

    usize namelen = strlen(dw->name);

    isize p = string_lastindexof(dw->name, namelen, '.');
    if (p <= 0 || (usize)p + 1 == namelen)
      continue; // ignore e.g. "a", ".a", "a."

    const char* ext = &dw->name[p+1];
    if (!strieq(ext, "co") && !strieq(ext, "c"))
      continue; // ignore e.g. "a.x"

    srcfile_t* f = pkg_add_srcfile(pkg, dw->name);
    if (!f) {
      err = ErrNoMem;
      break;
    }
    struct stat* st = dirwalk_stat(dw);
    f->mtime = unixtime_of_stat_mtime(st);
    f->size = (usize)st->st_size;
  }

  dirwalk_close(dw);

  return err;
}


unixtime_t pkg_source_mtime(const pkg_t* pkg) {
  unixtime_t mtime_max = 0;
  for (u32 i = 0; i < pkg->files.len; i++)
    mtime_max = MAX(mtime_max, pkg->files.v[i].mtime);
  return mtime_max;
}


// pkg_unit_srcdir returns the absolute path to a unit's source directory.
// I.e. "/a/b/foo" for srcfile "foo/bar.co" in pkg "/a/b"
str_t pkg_unit_srcdir(const pkg_t* pkg, const unit_t* unit) {
  assertf(path_isabs(pkg->dir.p), "pkg.dir \"%s\" is not absolute", pkg->dir.p);
  assertf(({
    str_t pkgdir = str_makelen(pkg->dir.p, pkg->dir.len);
    assert(pkgdir.len > 0);
    path_clean(&pkgdir);
    int d = strcmp(pkgdir.p, pkg->dir.p);
    str_free(pkgdir);
    d == 0;
  }), "pkg.dir \"%s\" is not clean", pkg->dir.p);

  str_t dir = path_join(pkg->dir.p, unit->srcfile->name.p, "..");
  if LIKELY(dir.len > 0) {
    isize i = string_lastindexof(dir.p, dir.len, PATH_SEP);
    if (i == 0) {
      dir.len = 1;
    } else {
      assert(i > 0); // pkg->dir is always an absolute path
      dir.len = (usize)i;
    }
  }
  return dir;
}


// pkg_resolve_dir updates pkg->dir
static err_t pkg_resolve_dir(pkg_t* pkg, const char* parentdir) {
  str_t fspath;

  #ifdef WIN32
    str_t path = str_makelen(pkg->path.p, pkg->path.len);
    if (path.cap == 0)
      return ErrNoMem;
    str_replacec(&path, '/', '\\'/*PATH_SEP*/, -1);
    if (pkg->path.p[0] != '.') {
      // e.g. "foo/bar"
      fspath = path;
    } else {
      fspath = path_join(parentdir, path.p);
      str_free(path);
    }
  #else
    if (pkg->path.p[0] != '.') {
      // e.g. "foo/bar"
      fspath = str_makelen(pkg->path.p, pkg->path.len);
    } else {
      fspath = path_join(parentdir, pkg->path.p);
    }
  #endif

  if (fspath.cap == 0)
    return ErrNoMem;

  usize rootdirlen;
  err_t err = import_resolve_fspath(&fspath, &rootdirlen);
  if UNLIKELY(err) {
    str_free(fspath);
    return err;
  }

  if (rootdirlen == 0)
    rootdirlen = strlen(parentdir);

  pkg->dir = fspath;
  pkg->rootdir = str_slice(fspath, 0, rootdirlen);

  return 0;
}


static err_t pkg_set_path_from_dir(pkg_t* pkg) {
  assert(pkg->dir.len > 0);
  assert(path_isabs(pkg->dir.p));

  // set path to basename of dir, e.g. dir="/a/b/c" -> rootdir="/a/b" path="c"
  pkg->path.len = 0;
  bool ok;
  const char* base = path_base(pkg->dir.p); // e.g. "c" in "/a/b/c"
  if (path_isabs(base)) {
    // pkg->dir is file system root, e.g. "/" or "C:"
    // e.g. dir="/" -> rootdir="/" path="main"
    pkg->rootdir = str_slice(pkg->dir);
    ok = str_append(&pkg->path, "main");
  } else {
    // e.g. dir="/a/b/c" -> rootdir="/a/b" path="c"
    usize dirnamelen = (usize)(uintptr)(base - pkg->dir.p) - 1;
    pkg->rootdir = str_slice(pkg->dir, 0, dirnamelen);
    ok = str_append(&pkg->path, base);
  }

  if LIKELY(ok)
    return 0;

  str_free(pkg->path);
  str_free(pkg->dir);
  pkg->dir = (str_t){0};
  pkg->path = (str_t){0};
  pkg->rootdir = (slice_t){0};
  return ErrNoMem;
}


static err_t pkg_resolve_toplevel_cwd(pkg_t* pkg) {
  pkg->dir = path_cwd();
  if (pkg->dir.cap == 0)
    return ErrNoMem;
  return pkg_set_path_from_dir(pkg);
}


static err_t pkg_resolve_toplevel(pkg_t* pkg, const char* import_path, mode_t st_mode) {
  assertf(*import_path != 0, "empty path");
  pkg->path = str_make(import_path);
  safecheckx(path_clean(&pkg->path)); // e.g. "./my//package" => "my/package"

  // current directory (".")
  if (pkg->path.p[0] == '.' && pkg->path.p[1] == 0) {
    assert(pkg->path.len == 1); // path_clean only results in "." (never e.g. "./x")
    return pkg_resolve_toplevel_cwd(pkg);
  }

  err_t err = 0;

  // absolute path, e.g. "/foo/bar"
  if (path_isabs(pkg->path.p)) {
    pkg->dir = pkg->path;
    pkg->path = (str_t){0};
    return pkg_set_path_from_dir(pkg);
  }

  // parent-relative path, e.g. "../bar"
  if (pkg->path.p[0] == '.') {
    // note: path_clean ensures that "./foo/bar" => "foo/bar"
    assert(pkg->path.p[0] != '.' || pkg->path.p[1] == '.');
    // TODO
  }

  if (S_ISDIR(st_mode)) {
    pkg->dir = str_makelen(pkg->path.p, pkg->path.len);
    path_makeabs(&pkg->dir);
    return 0;
  }

  // symbolic, to be found in copath, e.g. "foo/bar"
  str_t cwd = path_cwd();
  err = pkg_resolve_dir(pkg, cwd.p);
  str_free(cwd);

  if (!err)
    return 0;

  elog("%s: cannot find package %s", coprogname, pkg->path.p);
  str_free(pkg->path);
  str_free(pkg->dir);
  pkg->path = (str_t){0};
  pkg->dir = (str_t){0};
  pkg->rootdir = (slice_t){0};
  return ErrNoMem;
}


err_t pkgs_for_argv(int argc, char* argv[], pkg_t** pkgvp, u32* pkgcp) {
  // input arguments are either files OR directories:
  // - files builds one ad-hoc package e.g. "build foo/a.co bar/b.c"
  // - directories builds packages e.g. "build foo bar"

  err_t err = 0;
  u32 pkgc = 0;
  memalloc_t ma = memalloc_ctx();

  // check inputs
  struct stat* stv = mem_alloctv(ma, struct stat, (usize)argc);
  safecheckf(stv, "out of memory");
  u8 input_type = 0;
  for (int i = 0; i < argc; i++) {
    if (stat(argv[i], &stv[i]) != 0 || S_ISDIR(stv[i].st_mode)) {
      // directory or package name; guess from filename
      if (filetype_guess(argv[i]) == FILE_OTHER) {
        input_type |= 2;
      } else {
        input_type |= 1;
      }
    } else if (S_ISREG(stv[i].st_mode)) {
      // files as input; build one ad-hoc package
      input_type |= 1;
    } else {
      elog("%s: unsupported input file type", argv[i]);
      err = ErrNotSupported;
      goto end;
    }
    if (input_type == (1 | 2)) {
      elog("mixing files and directories as inputs makes me confused!");
      err = ErrInvalid;
      goto end;
    }
  }

  // allocate pkg_t structs
  pkgc = (input_type == 1) ? 1 : (u32)argc;
  pkg_t* pkgv = mem_alloctv(ma, pkg_t, pkgc);
  *pkgvp = pkgv;
  if (!pkgv) {
    err = ErrNoMem;
    goto end;
  }
  for (u32 i = 0; i < pkgc; i++) {
    if (( err = pkg_init(&pkgv[i], ma) )) {
      dlog("pkg_init failed");
      goto end;
    }
  }

  // ad-hoc main package of one or more files
  if (input_type == 1) {
    if (( err = pkg_resolve_toplevel_cwd(pkgv) ))
      goto end;
    pkgv->isadhoc = true;
    for (int i = 0; i < argc; i++) {
      srcfile_t* f = pkg_add_srcfile(pkgv, argv[i]);
      f->mtime = unixtime_of_stat_mtime(&stv[i]);
      f->size = (usize)stv[i].st_size;
    }
    goto end;
  }

  // multiple packages
  str_t cwd = {0};
  for (int i = 0; !err && i < argc; i++) {
    if UNLIKELY(*argv[i] == 0) {
      elog("%s: argument %d: empty package name", coprogname, i);
      err = ErrInvalid;
    } else {
      pkg_t* pkg = &pkgv[i];
      err = pkg_resolve_toplevel(pkg, argv[i], stv[i].st_mode);
      if (!err)
        assertf(path_isabs(pkg->dir.p), "pkg.dir \"%s\" is not absolute", pkg->dir.p);
    }
  }

  str_free(cwd);

end:
  if (err && pkgv) {
    for (u32 i = 0; i < pkgc; i++)
      pkg_dispose(&pkgv[i], ma);
  }
  mem_freetv(ma, stv, (usize)argc);
  *pkgcp = pkgc;
  return err;
}


static str_t _pkg_builddir(const pkg_t* pkg, const compiler_t* c, usize extracap) {
  // pkgbuilddir = {builddir}/pkg/{pkgname}
  slice_t basedir = slice_cstr(c->builddir);
  slice_t prefix = slice_cstr("pkg");

  str_t s = str_makeempty(basedir.len + 1 + pkg->path.len + 1 + prefix.len + extracap);
  safecheck(s.p != NULL);

  str_appendlen(&s, basedir.p, basedir.len);
  str_push(&s, PATH_SEP);
  str_appendlen(&s, prefix.p, prefix.len);
  str_push(&s, PATH_SEP);
  str_appendlen(&s, pkg->path.p, pkg->path.len);

  return s;
}


str_t pkg_builddir(const pkg_t* pkg, const compiler_t* c) {
  return _pkg_builddir(pkg, c, 0);
}


str_t pkg_buildfile(const pkg_t* pkg, const compiler_t* c, const char* filename) {
  usize filename_len = strlen(filename);
  str_t apifile = _pkg_builddir(pkg, c, 1 + filename_len);
  str_push(&apifile, PATH_SEP);
  str_appendlen(&apifile, filename, filename_len);
  return apifile;
}


bool pkg_is_built(const pkg_t* pkg, const compiler_t* c) {
  // str_t statusfile = path_join(c->pkgbuilddir, "status.toml");
  // unixtime_t status_mtime = fs_mtime(statusfile.p);
  // unixtime_t source_mtime = pkg_source_mtime(pkg);

  // TODO: Read "default_outfile" from statusfile and check its mtime and existence
  //       Use const char* opt_out if set.

  dlog("TODO: pkg_is_built implementation");

  return false;
}


node_t* nullable pkg_def_get(pkg_t* pkg, sym_t name) {
  node_t* n = NULL;
  rwmutex_rlock(&pkg->defs_mu);
  void** vp = map_lookup_ptr(&pkg->defs, name);
  if (vp)
    n = *vp;
  rwmutex_runlock(&pkg->defs_mu);
  return n;
}


err_t pkg_def_set(pkg_t* pkg, memalloc_t ma, sym_t name, node_t* n) {
  err_t err = 0;
  rwmutex_lock(&pkg->defs_mu);
  void** vp = map_assign_ptr(&pkg->defs, ma, name);
  if UNLIKELY(!vp) {
    err = ErrNoMem;
  } else {
    *vp = n;
  }
  rwmutex_unlock(&pkg->defs_mu);
  return err;
}


err_t pkg_def_add(pkg_t* pkg, memalloc_t ma, sym_t name, node_t** np_inout) {
  err_t err = 0;
  rwmutex_lock(&pkg->defs_mu);
  void** vp = map_assign_ptr(&pkg->defs, ma, name);
  if UNLIKELY(!vp) {
    err = ErrNoMem;
  } else if (*vp) {
    *np_inout = *vp;
  } else {
    *vp = assertnotnull(*np_inout);
  }
  rwmutex_unlock(&pkg->defs_mu);
  return err;
}

