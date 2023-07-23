// SPDX-License-Identifier: Apache-2.0
#include "colib.h"
#include "compiler.h"
#include "path.h"
#include "dirwalk.h"

#include <sys/stat.h>
#include <err.h>


err_t pkg_init(pkg_t* pkg, memalloc_t ma) {
  // *pkg is assumed to be zeroed
  assertf(memcmp(pkg, &(pkg_t){0}, sizeof(*pkg)) == 0, "pkg not zeroed");
  err_t err;

  if (( err = rwmutex_init(&pkg->defs_mu) ))
    return err;
  if (( err = future_init(&pkg->loadfut) ))
    goto end_err1;
  if (( err = map_init(&pkg->defs, ma, 32) ? 0 : ErrNoMem ))
    goto end_err2;
  if (( err = typefuntab_init(&pkg->tfundefs, ma) ))
    goto end_err3;
  return 0;

end_err3:
  map_dispose(&pkg->defs, ma);
end_err2:
  future_dispose(&pkg->loadfut);
end_err1:
  rwmutex_dispose(&pkg->defs_mu);
  return err;
}


void pkg_dispose(pkg_t* pkg, memalloc_t ma) {
  str_free(pkg->path);
  str_free(pkg->dir);
  str_free(pkg->root);
  srcfilearray_dispose(&pkg->srcfiles);
  ptrarray_dispose(&pkg->imports, ma);
  if (pkg->defs.cap != 0)
    map_dispose(&pkg->defs, ma);
  rwmutex_dispose(&pkg->defs_mu);
  typefuntab_dispose(&pkg->tfundefs, ma);
}


srcfile_t* pkg_add_srcfile(pkg_t* pkg, const char* name, usize namelen, bool* addedp) {
  srcfile_t* f = srcfilearray_add(&pkg->srcfiles, name, namelen, addedp);
  if (f)
    f->pkg = pkg;
  return f;
}


err_t pkg_find_files(pkg_t* pkg) {
  if (pkg->dir.len == 0)
    return ErrNotFound;

  memalloc_t ma = memalloc_ctx();
  dirwalk_t* dw = dirwalk_open(ma, pkg->dir.p, 0);
  if (!dw)
    return ErrNoMem;

  if (pkg->srcfiles.len > 0) {
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

    bool added;
    srcfile_t* f = pkg_add_srcfile(pkg, dw->name, namelen, &added);
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
  for (u32 i = 0; i < pkg->srcfiles.len; i++)
    mtime_max = MAX(mtime_max, pkg->srcfiles.v[i].mtime);
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

  usize rootlen;
  err_t err = import_resolve_fspath(&fspath, &rootlen);
  if UNLIKELY(err) {
    str_free(fspath);
    return err;
  }

  if (rootlen == 0)
    rootlen = strlen(parentdir);

  pkg->dir = fspath;
  pkg->root = str_makelen(fspath.p, rootlen);
  if (pkg->root.len < rootlen)
    return ErrNoMem;

  // dlog("pkg_resolve_dir:");
  // dlog("  .path = \"%s\"", pkg->path.p);
  // dlog("  .dir  = \"%s\"", pkg->dir.p);
  // dlog("  .root = \"%.*s\"", (int)pkg->root.len, pkg->root.chars);

  return 0;
}


// pkg_set_path_from_dir sets pkg->path & pkg->root based on pkg->dir
static err_t pkg_set_path_from_dir(pkg_t* pkg) {
  assert(pkg->dir.len > 0);
  assert(path_isabs(pkg->dir.p));

  // set path to basename of dir, e.g. dir="/a/b/c" -> root="/a/b" path="c"
  pkg->path.len = 0;
  bool ok;
  const char* base = path_base_cstr(pkg->dir.p); // e.g. "c" in "/a/b/c"
  if (path_isabs(base)) {
    // pkg->dir is file system root, e.g. "/" or "C:"
    // e.g. dir="/" -> root="/" path="main"
    pkg->root = str_copy(pkg->dir);
    ok = str_append(&pkg->path, "main");
  } else {
    // e.g. dir="/a/b/c" -> root="/a/b" path="c"
    usize dirnamelen = (usize)(uintptr)(base - pkg->dir.p) - 1;
    pkg->root = str_makelen(pkg->dir.p, dirnamelen);
    ok = pkg->root.len == dirnamelen;
    ok &= str_append(&pkg->path, base);
  }

  if LIKELY(ok)
    return 0;

  str_free(pkg->path);
  str_free(pkg->dir);
  str_free(pkg->root);
  pkg->dir = (str_t){0};
  pkg->path = (str_t){0};
  pkg->root = (str_t){0};
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

  #ifdef WIN32
    // TODO: handle Windows paths. Something like this somewhere:
    // str_replacec(&pkg->path, '\\', '/', -1);
    elog("warning: Windows support is work-in-progress");
  #endif

  // current directory (".")
  if (pkg->path.p[0] == '.' && pkg->path.p[1] == 0) {
    assert(pkg->path.len == 1); // path_clean only results in "." (never e.g. "./x")
    return pkg_resolve_toplevel_cwd(pkg);
  }

  // absolute path, e.g. "/foo/bar"
  if (path_isabs(pkg->path.p)) {
    pkg->dir = pkg->path;
    pkg->path = (str_t){0};
    return pkg_set_path_from_dir(pkg);
  }

  // // parent-relative path, e.g. "../bar"
  // if (pkg->path.p[0] == '.') {
  //   // import_path is parent-relative path, e.g. "../bar"
  //   // Note that path_clean ensures that "./foo" => "foo", so even if
  //   // the user invokes compis with "./foo" we see "foo".
  //   assert(pkg->path.p[0] != '.' || pkg->path.p[1] == '.');
  // }

  err_t err = 0;

  // relative import of a directory that was found, e.g. "./foo"
  if (*import_path == '.') {
    if (st_mode == 0) {
      elog("%s: not found: %s", coprogname, import_path);
      return ErrNotFound;
    }

    pkg->dir = path_abs(pkg->path.p);
    if (pkg->dir.len == 0)
      return ErrNoMem;

    if (pkg->path.p[0] == '.') {
      // import_path is parent-relative, e.g. "../bar".
      // Note that path_clean ensures that "./foo" => "foo",
      // so even when the user invokes compis with "./foo" we see path="foo".
      pkg->path = (str_t){0};
      return pkg_set_path_from_dir(pkg);
    }

    // root = dir[0:len(dir)-len(path)-1]
    // e.g. dir="/a/b/c/d" path="c/d" root="/a/b"
    usize rootlen = pkg->dir.len - pkg->path.len;
    if (rootlen < 2) {
      rootlen = 1;
      pkg->root = str_make("/"); // TODO: Windows
    } else {
      rootlen--; // exclude the path separator
      pkg->root = str_makelen(pkg->dir.p, rootlen);
    }
    if (pkg->root.len != rootlen) {
      err = ErrNoMem;
      goto error;
    }

    // make sure we set the correct root
    assert(streq(path_join(pkg->root.p, pkg->path.p).p, pkg->dir.p));
    return 0;
  }

  // symbolic, to be found in copath, e.g. "foo/bar"
  str_t cwd = path_cwd();
  err = pkg_resolve_dir(pkg, cwd.p);
  str_free(cwd);

  if (!err)
    return 0;
  elog("%s: cannot find package %s", coprogname, pkg->path.p);

error:
  str_free(pkg->path);
  str_free(pkg->dir);
  str_free(pkg->root);
  pkg->path = (str_t){0};
  pkg->dir = (str_t){0};
  pkg->root = (str_t){0};
  return err;
}


// pkg_resolve_adhoc resolves an "ad-hoc" package made up of individually-provided
// source files.
// The path of the package is the path in between cwd and the common path prefix of
// all source files.
// If source files are outside of cwd, the package's path will be basename(cwd) and
// its root dirname(cwd).
// If a single source file is provided, the source file's name sans extension is
// appendend to the path.
// e.g.
//   co build foo/bar/a.co foo/bar/b.co  =>  pkg_t{path=bar, root=foo}
//   co build foo/bar/a.co               =>  pkg_t{path=bar/a, root=foo}
static err_t pkg_resolve_adhoc(
  pkg_t* pkg, const char*const filenamev[], struct stat filestv[], u32 filec)
{
  // before we do anything else, check if the files were found when we stat()ed them
  for (u32 i = 0; i < filec; i++) {
    if (filestv[i].st_mode == 0) {
      elog("%s: not found", filenamev[i]);
      return ErrNotFound;
    }
  }

  if ((usize)filec > USIZE_MAX/(sizeof(str_t) + sizeof(void*)))
    return ErrOverflow;

  // before searching for a common directory, we need to make the filenames absolute
  usize abspath_size = (sizeof(str_t) + sizeof(void*)) * filec;
  const char** abspathv = mem_alloc(memalloc_ctx(), abspath_size).p;
  if (!abspathv)
    return ErrNoMem;
  str_t* abspath_strv = (void*)abspathv + filec*sizeof(void*);
  err_t err = 0;
  for (u32 i = 0; i < filec; i++) {
    abspath_strv[i] = path_abs(filenamev[i]);
    if (abspath_strv[i].cap == 0) {
      err = ErrNoMem;
      break;
    }
    abspathv[i] = abspath_strv[i].p;
  }
  if (err)
    goto end;

  // find common path prefix to use for inferring package directory
  usize dirlen = path_common_dirname(abspathv, filec);
  const char* dir = abspathv[0];

  // start by setting dir to cwd
  pkg->dir = path_cwd();
  if (pkg->dir.cap == 0) {
    err = ErrNoMem;
    goto end;
  }

  // compare common dir to cwd
  bool is_dir_in_cwd = string_startswithn(dir, dirlen, pkg->dir.p, pkg->dir.len);
  if (0 && is_dir_in_cwd && dirlen > pkg->dir.len) {
    // sourcefile common dir is a subdirectory of cwd
    usize cwdlen = pkg->dir.len;
    pkg->root = pkg->dir; // move str ownership
    pkg->dir = str_makelen(dir, dirlen);
    pkg->path = str_makelen(dir + cwdlen + 1, dirlen - cwdlen - 1);
  } else {
    // either the common srcfile dir is outside cwd or it is cwd
    // note: dir==cwd at this point
    // if (( err = pkg_set_path_from_dir(pkg) ))
    //   return err;

    // set dir to common directory of srcfiles
    // set root to dirname(dir)
    // set path to basename(dir)
    pkg->dir.len = 0;
    str_appendlen(&pkg->dir, dir, dirlen);
    pkg->root = str_makelen(dir, path_dir_len(dir, dirlen));
    const char* path = path_basen(dir, &dirlen); // note: updates dirlen
    pkg->path = str_makelen(path, dirlen);
  }

  if (pkg->dir.cap == 0 || pkg->path.cap == 0 || pkg->root.cap == 0) {
    err = ErrNoMem;
    goto end;
  }

  // append single source file name (sans extension) to path
  if (filec == 1) {
    const char* filename = filenamev[0];
    usize len = strlen(filename);
    filename = path_basen(filename, &len);
    isize i = string_lastindexof(filename, len, '.');
    if (i > 0)
      len = (usize)i;
    if (!str_push(&pkg->path, '/') || !str_appendlen(&pkg->path, filename, len)) {
      err = ErrNoMem;
      goto end;
    }
  }

  // add srcfiles
  for (u32 i = 0; i < filec; i++) {
    assert(pkg->dir.len+1 < strlen(abspathv[i]));
    assert(string_startswith(abspathv[i], pkg->dir.p));

    // use path relative to pkg->dir
    const char* path = abspathv[i] + pkg->dir.len + 1;
    usize pathlen = strlen(abspathv[i]) - pkg->dir.len - 1;

    srcfile_t* f = pkg_add_srcfile(pkg, path, pathlen, /*addedp*/NULL);
    if (!f) {
      err = ErrNoMem;
      break;
    }

    f->mtime = unixtime_of_stat_mtime(&filestv[i]);
    f->size = (usize)filestv[i].st_size;
  }

end:
  mem_freex(memalloc_ctx(), MEM(abspathv, abspath_size));
  return err;
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
    if (stat(argv[i], &stv[i]) != 0) {
      // stat failed; guess type from filename
      memset(&stv[i], 0, sizeof(stv[i]));
      if (filetype_guess(argv[i]) == FILE_OTHER) {
        input_type |= 2;
      } else {
        input_type |= 1;
      }
    } else if (S_ISDIR(stv[i].st_mode)) {
      // directory
      input_type |= 2;
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
    err = pkg_resolve_adhoc(pkgv, (const char**)argv, stv, (u32)argc);
    goto end;
  }

  // multiple packages
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

end:
  if (err && pkgv) {
    for (u32 i = 0; i < pkgc; i++)
      pkg_dispose(&pkgv[i], ma);
  }
  mem_freetv(ma, stv, (usize)argc);
  *pkgcp = pkgc;
  return err;
}


static bool _append_pkg_builddir(
  const pkg_t* pkg, const compiler_t* c, str_t* dst, usize extracap)
{
  // pkgbuilddir = {builddir}/pkg/{pkgname}
  slice_t basedir = slice_cstr(c->builddir);
  slice_t prefix = slice_cstr("pkg");

  usize nbyte = basedir.len + 1 + pkg->path.len + 1 + prefix.len + extracap;
  if (!str_ensure_avail(dst, nbyte))
    return false;

  str_appendlen(dst, basedir.p, basedir.len);
  str_push(dst, PATH_SEP);
  str_appendlen(dst, prefix.p, prefix.len);
  str_push(dst, PATH_SEP);
  str_appendlen(dst, pkg->path.p, pkg->path.len);
  return true;
}


bool pkg_builddir(const pkg_t* pkg, const compiler_t* c, str_t* dst) {
  return _append_pkg_builddir(pkg, c, dst, 0);
}


bool pkg_buildfile(
  const pkg_t* pkg, const compiler_t* c, str_t* dst, const char* filename)
{
  usize filename_len = strlen(filename);
  if (!_append_pkg_builddir(pkg, c, dst, 1 + filename_len))
    return false;
  str_push(dst, PATH_SEP);
  str_appendlen(dst, filename, filename_len);
  return true;
}


bool pkg_libfile(const pkg_t* pkg, const compiler_t* c, str_t* dst) {
  #define PKG_LIBFILE_PREFIX "lib"
  #define PKG_LIBFILE_SUFFIX ".a"

  // note: not PATH_SEP but "/" since pkg path is always POSIX style
  isize slashi = string_indexof(pkg->path.p, pkg->path.len, '/') + 1;
  usize extracap = 1  // "/"
                 + strlen(PKG_LIBFILE_PREFIX)
                 + (pkg->path.len - (usize)slashi)
                 + strlen(PKG_LIBFILE_SUFFIX) ;
  if (!_append_pkg_builddir(pkg, c, dst, extracap))
    return false;
  str_append(dst, PATH_SEP_STR PKG_LIBFILE_PREFIX);
  str_appendlen(dst, pkg->path.p + slashi, pkg->path.len - (usize)slashi);
  str_append(dst, PKG_LIBFILE_SUFFIX);
  return true;
}


bool pkg_exefile(const pkg_t* pkg, const compiler_t* c, str_t* dst) {
  #define PKG_EXEDIRNAME "bin"

  isize slashi = string_indexof(pkg->path.p, pkg->path.len, '/') + 1;

  slice_t builddir = slice_cstr(c->builddir);
  slice_t suffix = {0};
  // TODO: Windows target:
  // if (c->target.sys == SYS_win32)
  //   suffix = slice_cstr(".exe");

  usize nbyte = builddir.len
              + 1 + strlen(PKG_EXEDIRNAME)
              + 1 + (pkg->path.len - (usize)slashi)
              + suffix.len;

  if (!str_ensure_avail(dst, nbyte))
    return false;

  // e.g. builddir/bin/foo.exe
  str_appendlen(dst, builddir.chars, builddir.len);
  str_append(dst, PATH_SEP_STR PKG_EXEDIRNAME PATH_SEP_STR);
  str_appendlen(dst, pkg->path.p + slashi, pkg->path.len - (usize)slashi);
  str_appendlen(dst, suffix.chars, suffix.len);
  return true;
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


bool pkg_imports_add(pkg_t* importer_pkg, pkg_t* dep, memalloc_t ma) {
  return ptrarray_sortedset_addptr(&importer_pkg->imports, ma, dep);
}


bool pkg_dir_of_root_and_path(str_t* dst, slice_t root, slice_t path) {
  usize dst_len = dst->len;
  bool ok = str_ensure_avail(dst, root.len + 1 + path.len);

  ok &= str_appendlen(dst, root.chars, root.len);
  ok &= str_push(dst, PATH_SEP);

  #ifdef WIN32
    str_replacec(&path, '/', '\\', -1);
    ok &= str_appendlen(dst, path.chars, path.len);
    str_replacec(&path, '\\', '/', -1);
  #else
    ok &= str_appendlen(dst, path.chars, path.len);
  #endif

  if (!ok)
    dst->len = dst_len;
  return ok;
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

