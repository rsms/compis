// loc_t is a compact representation of a source location: file, line, column & width.
// Inspired by the Go compiler's xpos & lico. (loc_t)0 is invalid.
// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "str.h"
#include "array.h"
ASSUME_NONNULL_BEGIN

typedef u8 filetype_t;
enum filetype {
  FILE_OTHER,
  FILE_O,
  FILE_C,
  FILE_CO,
};

typedef struct pkg_ pkg_t;

typedef struct srcfile_ {
  pkg_t* nullable      pkg;    // parent package (set by pkg_add_srcfile)
  str_t                name;   // relative to pkg.dir (or absolute if there's no pkg.dir)
  const void* nullable data;   // NULL until srcfile_open (and NULL after srcfile_close)
  usize                size;   // byte size of data, set by pkg_find_files, pkgs_for_argv
  unixtime_t           mtime;  // modification time, set by pkg_find_files, pkgs_for_argv
  bool                 ismmap; // true if srcfile_open used mmap
  filetype_t           type;   // file type (set by pkg_add_srcfile)
} srcfile_t;


err_t srcfile_open(srcfile_t* sf);
void srcfile_close(srcfile_t* sf);
void srcfile_dispose(srcfile_t* sf);
srcfile_t* nullable srcfilearray_add(
  ptrarray_t* srcfiles, const char* name, usize namelen, bool* nullable addedp);
void srcfilearray_dispose(ptrarray_t* srcfiles);


filetype_t filetype_guess(const char* filename);


ASSUME_NONNULL_END
