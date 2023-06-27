// dirwalk performs file directory tree traversal
// SPDX-License-Identifier: Apache-2.0
//
// Traversal properties:
// - directories always visited before their files
// - subdirectories are traversed after all files in the current dir has been visited
// - order of results is undefined (i.e. the file system driver decides)
//
#pragma once
#include <sys/stat.h>
ASSUME_NONNULL_BEGIN

typedef struct {
  memalloc_t  ma;
  err_t       err;
  mode_t      type; // current entry type; a stat S_IF constant (S_IFDIR, S_IFREG etc)
  const char* name; // current name (e.g. "cat.txt")
  const char* path; // current path (e.g. "/foo/bar/cat.txt")
  // + internal fields
} dirwalk_t;

// dirwalk_open creates a directory walker for directory at dirpath.
// Returns NULL if memory allocation failed.
dirwalk_t* nullable dirwalk_open(memalloc_t ma, const char* dirpath, int flags);

// dirwalk_close disposes of a directory walker
void dirwalk_close(dirwalk_t*);

// dirwalk_next reads the next entry.
// Returns: 1 entry found, 0 end, <0 error
err_t dirwalk_next(dirwalk_t*);

// dirwalk_lstat returns status of the current entry.
// Results are cached and may have been updated by dirwalk_next.
struct stat* dirwalk_lstat(dirwalk_t*);

// dirwalk_stat returns status of the current entry, resolving any symlinks.
// Results are cached and may have been updated by dirwalk_next.
struct stat* dirwalk_stat(dirwalk_t*);

// dirwalk_descend requests that the current entry be visited.
// Note: Has no effect unless type==S_IFDIR
void dirwalk_descend(dirwalk_t*);

// dirwalk_parent_path returns the path of the parent directory.
// If this function is called just after calling dirwalk_open, before any calls to
// dirwalk_next, it returns the path_clean()ed dirpath provided to dirwalk_open.
const char* dirwalk_parent_path(dirwalk_t*);


ASSUME_NONNULL_END
