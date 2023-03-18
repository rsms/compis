// SPDX-License-Identifier: Apache-2.0
//
// This is a quite simple implementation with the following properties:
// - at most one open directory at a time
// - minimizes memory usage via some trade offs:
//   - results are not sorted (order is whatever readdir yields)
//   - subdirectories are traversed after all files in a dir has been visited
//
// Example content (a-z)  │ Traversal order
// ───────────────────────┼──────────────────────────────
//  animals/              │ animals/
//    cats/               │   elephant.txt
//      jaguar.txt        │   zebra.txt
//      tiger.txt         │   cats/
//    elephant.txt        │     jaguar.txt
//    pets/               │     tiger.txt
//      cats/             │   pets/
//        joshi.txt       │     bob-the-ant.txt
//        monza.txt       │     cats/
//      bob-the-ant.txt   │       joshi.txt
//      dogs/             │       monza.txt
//        lars.txt        │     dogs/
//        snarf.txt       │       lars.txt
//    zebra.txt           │       snarf.txt
//
#include "colib.h"
#include "dirwalk.h"
#include "path.h"

#include <dirent.h>
#include <errno.h>


#if defined(__APPLE__) || defined(__linux__) || defined(__wasi__)
  // struct dirent has d_type field
  static_assert(sizeof(((struct dirent*)0)->d_type), ""); // let's make sure
  #define HAS_DIRENT_TYPE
#endif


typedef struct dirinfo {
  struct dirinfo* nullable next;
  usize pathlen;
  char  path[];
} dirinfo_t;

typedef struct {
  dirwalk_t           d;
  struct stat         st;       // status of current entry (uninitialized if st_nlink==0)
  DIR* nullable       dirp;     // current directory handle
  dirinfo_t* nullable dir;      // current directory info (null means "dequeue a dir")
  dirinfo_t* nullable dirstack; // enqueued directories, to visit
  char                pathbuf[PATH_MAX*2];
} dirw0_t;


static void dirinfo_free(dirw0_t* dw, dirinfo_t* dir) {
  mem_freex(dw->d.ma, MEM(dir, sizeof(*dir) + dir->pathlen + 1));
}


void dirwalk_descend(dirwalk_t* d) {
  if (d->type != S_IFDIR)
    return;

  dirw0_t* dw = (dirw0_t*)d;

  usize pathlen = strlen(d->path);
  dirinfo_t* dir = mem_alloc(d->ma, sizeof(dirinfo_t) + pathlen + 1).p;
  if (!dir) {
    d->err = ErrNoMem;
    return;
  }
  memcpy(dir->path, d->path, pathlen + 1);
  dir->pathlen = pathlen;
  dir->next = dw->dirstack;
  dw->dirstack = dir;
}


static bool open_next_dir(dirw0_t* dw) {
  // pop dir from queue, set it as current directory
  dw->dir = assertnotnull(dw->dirstack);
  dw->dirstack = dw->dirstack->next;

  if UNLIKELY((dw->dirp = opendir(dw->dir->path)) == NULL) {
    dw->d.err = err_errno();
    dlog("opendir %s: %s", dw->dir->path, err_str(dw->d.err));
    goto error;
  }

  if UNLIKELY(dw->dir->pathlen >= sizeof(dw->pathbuf)) {
    dw->d.err = ErrOverflow;
    goto error;
  }
  memcpy(dw->pathbuf, dw->dir->path, dw->dir->pathlen);
  dw->pathbuf[dw->dir->pathlen] = 0;
  dw->d.type = S_IFDIR;

  return true;

error:
  dirinfo_free(dw, dw->dir);
  dw->dir = NULL;
  if (dw->dirp) {
    closedir(dw->dirp);
    dw->dirp = NULL;
  }
  return false;
}


dirwalk_t* nullable dirwalk_open(memalloc_t ma, const char* dirpath, UNUSED int flags) {
  dirw0_t* dw = mem_alloc(ma, sizeof(dirw0_t)).p;
  if (!dw)
    return NULL;

  memset(dw, 0, sizeof(*dw) - sizeof(dw->pathbuf));
  dw->d.ma = ma;
  dw->d.name = "";
  dw->d.path = dw->pathbuf;
  dw->d.type = S_IFDIR;

  // clean & enqueue root directory
  usize pathlen = strlen(dirpath);
  path_cleanx(dw->pathbuf, sizeof(dw->pathbuf), dirpath, pathlen);
  dirwalk_descend(&dw->d);

  return &dw->d;
}


void dirwalk_close(dirwalk_t* d) {
  dirw0_t* dw = (dirw0_t*)d;

  for (dirinfo_t* dir = dw->dirstack; dir; ) {
    dirinfo_t* dir_next = dir->next;
    dirinfo_free(dw, dir);
    dir = dir_next;
  }

  if (dw->dirp)
    closedir(dw->dirp);

  mem_freet(dw->d.ma, dw);
}


err_t dirwalk_next(dirwalk_t* d) {
  dirw0_t* dw = (dirw0_t*)d;

  if (d->err)
    return d->err;

  if (dw->dir == NULL) {
    if (dw->dirstack == NULL)
      return 0; // end
    if (!open_next_dir(dw))
      return d->err;
  }

readdir_again:
  errno = 0;
  struct dirent* dent = readdir(dw->dirp);
  if (!dent) {
    // end of directory (or readdir error)
    d->err = err_errno();
    closedir(dw->dirp); dw->dirp = NULL;
    dirinfo_free(dw, dw->dir); dw->dir = NULL;
    MUSTTAIL return dirwalk_next(d);
  }

  // ignore "", "." and ".." entries
  if (*dent->d_name == 0 ||
      ( *dent->d_name == '.' &&
        (dent->d_name[1] == 0 || (dent->d_name[1] == '.' && dent->d_name[2] == 0)) )
  ) {
    goto readdir_again;
  }

  d->name = dent->d_name;

  // set d->path
  usize namelen = strlen(d->name);
  if (dw->dir->pathlen + 1 + namelen >= sizeof(dw->pathbuf))
    return d->err = ErrOverflow;
  dw->pathbuf[dw->dir->pathlen] = PATH_SEPARATOR;
  memcpy(&dw->pathbuf[dw->dir->pathlen + 1], d->name, namelen + 1);

  // set d->type
  #ifdef HAS_DIRENT_TYPE
    d->type = DTTOIF(dent->d_type);
    dw->st.st_nlink = 0; // mark as uninitialized
  #else
    if (lstat(d->path, &dw->st) != 0)
      return d->err = err_errno();
    d->type = dw->st.st_mode & S_IFMT;
  #endif

  return 1;
}


struct stat* dirwalk_lstat(dirwalk_t* d) {
  dirw0_t* dw = (dirw0_t*)d;
  if (dw->st.st_nlink == 0 && lstat(d->path, &dw->st) != 0)
    d->err = err_errno();
  return &dw->st;
}


const char* dirwalk_parent_path(dirwalk_t* d) {
  dirw0_t* dw = (dirw0_t*)d;
  if (dw->dir)
    return dw->dir->path;
  // This result is only valid directly after a call to dirwalk_open()
  // If dirwalk_next() returned <1 then the result is undefined (as is expected)
  return dw->pathbuf;
}
