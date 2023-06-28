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
  // err_t err = rwmutex_init(&pkg->mu);
  // if (err)
  //   return err;
  err_t err = 0;
  if (( err = map_init(&pkg->defs, ma, 32) ? 0 : ErrNoMem )) {
    // rwmutex_dispose(&pkg->mu);
    return err;
  }
  if (( err = map_init(&pkg->tfuns, ma, 32) ? 0 : ErrNoMem )) {
    map_dispose(&pkg->defs, ma);
    // rwmutex_dispose(&pkg->mu);
    return err;
  }
  return 0;
}


void pkg_dispose(pkg_t* pkg, memalloc_t ma) {
  str_free(pkg->name);
  str_free(pkg->dir);
  if (pkg->defs.cap == 0)
    return;
  // rwmutex_dispose(&pkg->mu);
  array_dispose(srcfile_t, (array_t*)&pkg->files, ma);
  map_dispose(&pkg->defs, ma);
  map_dispose(&pkg->tfuns, ma);
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

    isize p = slastindexofn(dw->name, namelen, '.');
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


bool pkg_find_dir(pkg_t* pkg) {
  str_t dir = {0};

  if (str_startswith(pkg->name, "std/")) {
    dir = path_join(coroot, "..", pkg->name.p);
    vlog("looking for pkg \"%s\" at \"%s\"", pkg->name.p, dir.p);
    if (fs_isdir(dir.p))
      goto found;
  }

  // try COPATH
  for (const char*const* copathp = copath; *copathp; copathp++) {
    dir.len = 0;
    str_append(&dir, *copathp);
    str_push(&dir, PATH_SEPARATOR);
    str_appendlen(&dir, pkg->name.p, pkg->name.len);
    if ((*copathp)[0] != '.') {
      safecheckx(path_makeabs(&dir));
    } else {
      path_clean(&dir);
    }
    vlog("looking for pkg \"%s\" at \"%s\"", pkg->name.p, dir.p);
    if (fs_isdir(dir.p))
      goto found;
  }

  // not found
  str_free(dir);
  return false;

found:
  if (pkg->dir.p)
    str_free(pkg->dir);
  pkg->dir = dir;
  return true;
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
      // directory or package name
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

  // ad-hoc main package?
  if (input_type == 1) {
    pkgv->name = str_make("main");
    pkgv->dir = str_make("");
    for (int i = 0; i < argc; i++) {
      srcfile_t* f = pkg_add_srcfile(pkgv, argv[i]);
      f->mtime = unixtime_of_stat_mtime(&stv[i]);
      f->size = (usize)stv[i].st_size;
    }
    goto end;
  }

  // multiple packages
  str_t cwd = {0};
  for (int i = 0; i < argc; i++) {
    pkg_t* pkg = &pkgv[i];

    pkg->name = str_make(argv[i]);
    safecheckx(path_clean(&pkg->name)); // e.g. "./my//package" => "my/package"
    // note: path_clean results in "." if the input is empty ("").
    // We check anyhow to avoid crash in case path_clean impl would change.
    if (pkg->name.len == 0) {
      elog("%s: argument %d: empty package name", coprogname, i);
      err = ErrInvalid;
      break;
    }

    if (pkg->name.p[0] == '.' && pkg->name.p[1] == 0) {
      // current directory; use the basename of cwd
      assert(pkg->name.len == 1); // path_clean only results in "." (never e.g. "./x")
      if (cwd.p == NULL)
        cwd = path_cwd();
      pkg->dir = pkg->name; // move ownership (replace pkg.name next...)
      const char* base = path_base(cwd.p);
      pkg->name = path_isabs(base) ? str_make("main") : str_make(base);
    } else if (path_isabs(pkg->name.p) || pkg->name.p[0] == '.') {
      // e.g. "/my/package" => "package"
      //      "../package"  => "package"
      //      "/"           => "main"
      if (pkg->name.p[0] == '.')
        assert(pkg->name.p[1] == '.'); // e.g. "../foo"
      pkg->dir = str_makelen(pkg->name.p, pkg->name.len);
      const char* base = path_base(pkg->name.p);
      pkg->name = path_isabs(base) ? str_make("main") : str_make(base);
    } else if (S_ISDIR(stv[i].st_mode)) {
      // e.g. "./my/package" => "my/package"
      pkg->dir = str_makelen(pkg->name.p, pkg->name.len);
    } else {
      // e.g. "std/runtime"
      if (!pkg_find_dir(pkg)) {
        elog("%s: cannot find package %s", coprogname, pkg->name.p);
        err = ErrNotFound;
        break;
      }
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
