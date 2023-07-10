// importing of packages
// SPDX-License-Identifier: Apache-2.0
#include "colib.h"
#include "compiler.h"
#include "path.h"

#include <string.h>


#ifdef DEBUG
  static void assert_path_is_clean(const char* path) {
    str_t s = str_make(path);
    assert(s.len > 0);
    assert(path_clean(&s));
    assertf(strcmp(s.p, path) == 0,
      "path \"%s\" is not clean (expected \"%s\")", path, s.p);
    str_free(s);
  }
#else
  #define assert_path_is_clean(path) ((void)0)
#endif


bool import_validate_path(const char* path, const char** errmsgp, usize* erroffsp) {
  *errmsgp = NULL;
  *erroffsp = 0;

  if (*path == 0) {
    *errmsgp = "empty path";
    return false;
  }

  if (*path == ' ') {
    *errmsgp = "leading whitespace";
    return false;
  }

  if (*path == '/') {
    *errmsgp = "absolute path";
    return false;
  }

  // if path starts with "." it must be "./" or "../"
  if UNLIKELY(*path == '.' && (path[1] != '/' && (path[1] != '.' || path[2] != '/'))) {
    if (path[1] == 0) {
      // "."
      *errmsgp = "cannot import a package into itself";
    } else {
      *errmsgp = "must start with \"./\" or \"../\" when first character is '.'";
      *erroffsp = 1;
    }
    return false;
  }

  // check for invalid or reserved characters
  for (const u8* p = (u8*)path; *p; p++) {
    u8 c = *p;
    if UNLIKELY(c <= ' ' || c == ':' || c == '\\' || c == '@') {
      if (*p == ' ') {
        // permit space U+0020 anywhere but at the beginning or end of path
        if (p[1]) // not the end
          continue;
        *erroffsp = (usize)(uintptr)(p - (u8*)path);
        *errmsgp = "trailing whitespace";
        return false;
      }
      *erroffsp = (usize)(uintptr)(p - (u8*)path);
      switch (c) {
        case '@':  *errmsgp = "'@' is a reserved character"; break;
        case '\\': *errmsgp = "use '/' as path separator, not '\\'"; break;
        default:   *errmsgp = "invalid character"; break;
      }
      return false;
    }
  }

  if (*path != '.') {
    // symbolic paths must not contain "../" or end with "/.."
    const char* p = strstr(path, "/../");
    if (!p) {
      usize pathlen = strlen(path);
      if (string_endswithn(path, pathlen, "/..", 3))
        p = path - 3;
    }
    if UNLIKELY(p) {
      *erroffsp = (usize)(uintptr)(p - path) + 1;
      *errmsgp = "parent-directory reference";
      return false;
    }
  }

  return true;
}


static err_t import_clean_path(
  const pkg_t* importer_pkg,
  const char*  importer_fsdir, // absolute fs directory of the unit importing `path`
  str_t*       path,           // in-out
  str_t*       fspath_out)     // out
{
  // examples
  //   import from NON-ad-hoc package "foo/bar":
  //     input, symbolic "a/b":
  //       importer_pkg.path    = "foo/bar"
  //       importer_pkg.rootdir = "C:\src\lolcat"
  //       importer_pkg.dir     = "C:\src\lolcat\foo\bar"
  //       importer_fsdir       = "C:\src\lolcat\foo\bar"
  //       import_path          = "a/b"
  //       >>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>
  //       import_path          = "a/b"
  //       import_fspath        = "a\b"  <—— to be resolved by searching copath
  //
  //     input, relative "./a/b":
  //       importer_pkg.path    = "foo/bar"
  //       importer_pkg.rootdir = "C:\src\lolcat"
  //       importer_pkg.dir     = "C:\src\lolcat\foo\bar"
  //       importer_fsdir       = "C:\src\lolcat\foo\bar"
  //       import_path          = "./a/b"
  //       >>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>
  //       import_path          = "foo/bar/a/b"
  //       import_fspath        = "C:\src\lolcat\foo\bar\a\b"
  //
  //     input, relative "../a/b":
  //       importer_pkg.path    = "foo/bar"
  //       importer_pkg.rootdir = "C:\src\lolcat"
  //       importer_pkg.dir     = "C:\src\lolcat\foo\bar"
  //       importer_fsdir       = "C:\src\lolcat\foo\bar"
  //       import_path          = "../a/b"
  //       >>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>
  //       import_path          = "foo/a/b"
  //       import_fspath        = "C:\src\lolcat\foo\a\b"
  //
  //   import from AD-HOC package "foo/bar": (e.g. from "co build C:\other\dir\file.co")
  //     input, symbolic "a/b":
  //       importer_pkg.path    = "lolcat"
  //       importer_pkg.rootdir = "C:\src"
  //       importer_pkg.dir     = "C:\src\lolcat"  <——— $PWD of compis process
  //       importer_fsdir       = "C:\other\dir"
  //       import_path          = "a/b"
  //       >>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>
  //       import_path          = "a/b"
  //       import_fspath        = "a\b"  <—— to be resolved by searching copath
  //
  //     input, relative "./a/b":
  //       importer_pkg.path    = "lolcat"
  //       importer_pkg.rootdir = "C:\src"
  //       importer_pkg.dir     = "C:\src\lolcat"
  //       importer_fsdir       = "C:\other\dir"  <——— note! dir of srcfile, not pkg
  //       import_path          = "./a/b"
  //       >>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>
  //       import_path          = "lolcat/a/b"
  //       import_fspath        = "C:\other\dir\a\b"
  //
  //     input, relative "../a/b":
  //       importer_pkg.path    = "lolcat"
  //       importer_pkg.rootdir = "C:\src"
  //       importer_pkg.dir     = "C:\src\lolcat"
  //       importer_fsdir       = "C:\other\dir"
  //       import_path          = "../a/b"
  //       >>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>
  //       error: invalid (goes above "lolcat")
  //
  assert(path->len > 0);
  assertf(*path->p != '/', "%s", path->p);
  assertf(path_isabs(importer_fsdir), "%s", importer_fsdir);
  assertf(path_isabs(importer_pkg->dir.p), "%s", importer_pkg->dir.p);

  // dlog("——————————————————————————");
  // dlog("importer_pkg.path    = \"%s\"", importer_pkg->path.p);
  // dlog("importer_pkg.rootdir = \"%.*s\"",
  //   (int)importer_pkg->rootdir.len, importer_pkg->rootdir.chars);
  // dlog("importer_pkg.dir     = \"%s\"", importer_pkg->dir.p);
  // dlog("importer_fsdir       = \"%s\"", importer_fsdir);
  // dlog("import_path          = \"%s\"", path->p);

  // fspath starts out as a copy of the import path
  str_t fspath = str_makelen(path->p, path->len);

  // on Windows, transpose '/' to '\', e.g. "foo/bar" => "foo\bar"
  #ifdef WIN32
  str_replacec(&fspath, '/', '\\', -1);
  #endif

  err_t err = 0;

  // if the path is not relative, we are done (e.g. "foo/bar")
  if (path->p[0] != '.') {
    *fspath_out = fspath;
    goto end;
  }

  // Relative import is relative to the importing srcfile's directory.
  // (Relative import as in e.g. "../foo/bar" or "./foo/bar")
  *fspath_out = path_join(importer_fsdir, fspath.p);
  if (fspath_out->cap == 0) {
    err = ErrNoMem;
    goto end;
  }

  // check if path goes above pkg.rootdir
  if UNLIKELY(!path_isrooted(str_slice(*fspath_out), importer_pkg->rootdir)) {
    dlog("error: import path \"%s\" would escape pkg.rootdir=\"%.*s\"",
      path->p, (int)importer_pkg->rootdir.len, importer_pkg->rootdir.chars);
    err = ErrInvalid;
    goto end;
  }

  // Now we know that fspath_out has prefix importer_pkg->rootdir.
  // This gives us the symbolic package path:
  usize rootdir_len = importer_pkg->rootdir.len + 1; // +1 for ending PATH_SEP
  path->len = 0;
  if (!str_appendlen(path, fspath_out->p + rootdir_len, fspath_out->len - rootdir_len)) {
    err = ErrNoMem;
    goto end;
  }

  // on Windows, transpose '\' to '/', e.g. "foo\bar" => "foo/bar"
  #ifdef WIN32
  str_replacec(&path, '\\', '/', -1);
  #endif

end:
  // dlog(">>> import_path   = \"%s\"", path->p);
  // dlog(">>> import_fspath = \"%s\"", fspath_out->p);
  if (err) {
    str_free(*fspath_out);
    *fspath_out = (str_t){0};
  }
  return err;
}


err_t import_resolve_fspath(str_t* fspath, usize* rootdirlen_out) {
  assert(fspath->len > 0);

  // fspath must be either "/ab/so/lute" or "sym/bol/ic", not "./rel/a/tive"
  assertf(fspath->p[0] != '.', "relative fspath \"%s\"", fspath->p);

  // fspath is assumed to have been path_clean'ed
  assert_path_is_clean(fspath->p);
  #ifdef DEBUG
  {
    assert(strlen(fspath->p) == fspath->len);
    str_t s = str_makelen(fspath->p, fspath->len);
    assert(s.len > 0);
    assert(path_clean(&s));
    assertf(strcmp(s.p, fspath->p) == 0,
      "fspath \"%s\" is not clean (expected \"%s\")", fspath->p, s.p);
    str_free(s);
  }
  #endif

  str_t tmpstr = {0};
  err_t err = 0;
  *rootdirlen_out = 0;

  // If path is absolute, e.g. "/ab/so/lute", we just check that the directory
  // exists and then we are done. Otherwise fspath is symbolic, e.g. "sym/bol/ic"
  if (path_isabs(fspath->p))
    goto checked_return;

  // special "std/" prefix
  if (str_startswith(*fspath, "std/")) {
    // note: coroot is guaranteed to be absolute and path_clean'ed (coroot_init)
    int fspathlen = (int)MIN(fspath->len, (usize)I32_MAX); // for vlog
    usize corootlen = strlen(coroot);
    isize i = string_lastindexof(coroot, corootlen, PATH_SEP);
    assert(i > -1);
    if (i != 0)
      corootlen = (usize)i;
    if UNLIKELY(!str_prependlen(fspath, coroot, corootlen + 1/*include NUL*/)) {
      // restore altered fspath
      memmove(fspath->p, fspath->p + (corootlen + 1), fspath->len - (corootlen + 1));
      err = ErrNoMem;
      goto end;
    }
    fspath->p[corootlen] = PATH_SEP;
    *rootdirlen_out = corootlen;
    vlog("looking for package \"%.*s\" at \"%s\"", fspathlen, fspath->p, fspath->p);
    goto checked_return;
  }

  // search COPATH
  // note: copath entries have been path_clean'ed and are never empty.
  for (const char*const* dirp = copath; *dirp;) {
    str_free(tmpstr);
    if (( tmpstr = path_join(*dirp, fspath->p) ).cap == 0 || !path_makeabs(&tmpstr)) {
      err = ErrNoMem;
      goto end;
    }
    vlog("looking for package \"%s\" at \"%s\"", fspath->p, tmpstr.p);
    if (fs_isdir(tmpstr.p)) {
      // note: fspath is symbolic and guaranteed by pkg_validate_path to not contain ".."
      *rootdirlen_out = tmpstr.len - fspath->len - 1;
      str_t tmp = *fspath;
      *fspath = tmpstr;
      str_free(tmp);
      return 0;
    }
    // try next or end now as "not found"
    if (++dirp == NULL)
      return ErrNotFound;
  }
  // note: for loop never breaks; we never get here

checked_return:
  err = ErrNotFound * (err_t)!fs_isdir(fspath->p);
end:
  str_free(tmpstr);
  return err;
}


static err_t import_resolve_pkg(
  compiler_t* c, const pkg_t* importer_pkg, slice_t path, str_t* fspath, pkg_t** result)
{
  assert(path.len > 0);
  assertf(path.chars[0] != '.',
    "import path \"%.*s\" is relative; it must go through import_clean_path first",
    (int)path.len, path.chars);

  assert(fspath->len > 0);
  assertf(fspath->p[0] != '.',
    "fspath \"%s\" is relative; it must go through import_clean_path first",
    fspath->p);

  // resolve absolute filesystem directory path, or return "not found"
  usize rootdirlen;
  err_t err = import_resolve_fspath(fspath, &rootdirlen);
  if (err) {
    *result = NULL;
    return err;
  }

  rwmutex_lock(&c->pkgindex_mu);

  // lookup or allocate in map
  pkg_t* pkg = NULL;
  pkg_t** pkgp = (pkg_t**)map_assign(&c->pkgindex, c->ma, fspath->p, fspath->len);
  if UNLIKELY(!pkgp) {
    err = ErrNoMem;
    goto end;
  }

  // if package is already resolved, we are done
  if ((pkg = *pkgp) != NULL)
    goto end;

  // add package
  if UNLIKELY((pkg = mem_alloct(c->ma, pkg_t)) == NULL) {
    err = ErrNoMem;
    goto end;
  }
  if UNLIKELY(pkg_init(pkg, c->ma) != 0) {
    mem_freet(c->ma, pkg);
    err = ErrNoMem;
    goto end;
  }
  if (rootdirlen == 0)
    rootdirlen = importer_pkg->rootdir.len;
  pkg->path = str_makelen(path.p, path.len);
  pkg->dir = str_makelen(fspath->p, fspath->len);
  pkg->rootdir = str_slice(pkg->dir, 0, rootdirlen);
  // dlog("adding pkg");
  // dlog("  .path    = \"%s\"", pkg->path.p);
  // dlog("  .dir     = \"%s\"", pkg->dir.p);
  // dlog("  .rootdir = \"%.*s\"", (int)pkg->rootdir.len, pkg->rootdir.chars);
  *pkgp = pkg;

end:
  *result = pkg;
  rwmutex_unlock(&c->pkgindex_mu);
  return err;
}


typedef struct {
  str_t           path;
  str_t           fspath;
  const import_t* im;
} pkgimp_t;


static int pkgimp_cmp(const pkgimp_t* a, const pkgimp_t* b, void* ctx) {
  return strcmp(a->fspath.p, b->fspath.p);
}


err_t import_find_pkgs(
  compiler_t* c,
  const pkg_t* importer_pkg,
  const unit_t*const* unitv, u32 unitc,
  ptrarray_t* pkgs)
{
  err_t err = 0;
  str_t importer_dir = {0};

  array_type(pkgimp_t) unique_imports = {0};

  // Build a list of package to import, sorted uniquely on path.
  // This serves multiple purposes:
  // - Reduce obviously-duplicate package paths imported by many
  //   source files of a package.
  // - Make import resolution deterministic by sorting the imports.
  //   This makes debugging easier and produces consistent error messages.
  //   Note that unitv is sorted by srcfile path, (virtue of pkg->files
  //   being sorted by srcfile path.) However, we still want to sort
  //   imports to remove the effects of source import-declaration order.
  //
  for (u32 i = 0; i < unitc; i++) {
    const unit_t* unit = unitv[i];

    str_free(importer_dir);
    importer_dir = pkg_unit_srcdir(importer_pkg, unit);
    if (importer_dir.len == 0)
      goto end_err_nomem;

    for (const import_t* im = unit->importlist; im; im = im->next_import) {
      // Import path is either "/ab/so/lute" or "sym/bol/ic", not "./rel/a/tive"

      str_t path = str_make(im->path);
      str_t fspath;
      err = import_clean_path(importer_pkg, importer_dir.p, &path, &fspath);
      if (err) {
        str_free(path);
        goto end;
      }

      // dlog("import \"%s\" from cwd \"%s\"", path.p, importer_dir.p);

      // insert into sorted, unique list
      pkgimp_t keyip = { .fspath=fspath, .im=im };
      pkgimp_t* ip = array_sortedset_assign(pkgimp_t,
        &unique_imports, c->ma, &keyip, (array_sorted_cmp_t)pkgimp_cmp, NULL);
      if (!ip) {
        str_free(path);
        str_free(fspath);
        goto end_err_nomem;
      }
      if (!ip->im) {
        ip->path = path;
        ip->fspath = fspath;
        ip->im = im;
      } else {
        str_free(path);
        str_free(fspath);
      }
    }
  }

  // dlog("unique_imports:");
  // for (u32 i = 0; i < unique_imports.len; i++)
  //   dlog("  %s", unique_imports.v[i].fspath.p);

  // resolve imports
  for (u32 i = 0; i < unique_imports.len; i++) {
    pkgimp_t* ip = &unique_imports.v[i];
    pkg_t* pkg;
    err_t err1 = import_resolve_pkg(
      c, importer_pkg, str_slice(ip->path), &ip->fspath, &pkg);
    if UNLIKELY(err1) {
      if (err == 0) err = err1;
      if (err1 != ErrNotFound)
        break;
      origin_t origin = origin_make(&c->locmap, ip->im->pathloc);
      report_diag(c, origin, DIAG_ERR, "package \"%s\" not found", ip->im->path);
      // keep going so we can report all missing packages, if there is more than one
    } else if (!ptrarray_sortedset_addptr(pkgs, c->ma, pkg)) {
      goto end_err_nomem;
    }
  }

  goto end;

end_err_nomem:
  err = ErrNoMem;

end:
  str_free(importer_dir);
  for (u32 i = 0; i < unique_imports.len; i++)
    str_free(unique_imports.v[i].fspath);
  array_dispose(pkgimp_t, (array_t*)&unique_imports, c->ma);

  return err;
}
