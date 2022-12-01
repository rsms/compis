// file paths
// SPDX-License-Identifier: Apache-2.0
#pragma once
ASSUME_NONNULL_BEGIN

#ifdef WIN32
  #define PATH_SEPARATOR     '\\'
  #define PATH_SEPARATOR_STR "\\"
  #define PATH_DELIMITER     ';'
  #define PATH_DELIMITER_STR ";"
#else
  #define PATH_SEPARATOR     '/'
  #define PATH_SEPARATOR_STR "/"
  #define PATH_DELIMITER     ':'
  #define PATH_DELIMITER_STR ":"
#endif

// path_diroffs returns the position of the end of the directory part of path.
// E.g. "foo/bar/baz" => 7, "foo/" => 3, "foo" => 0.
// Returns 0 if path does not contain a directory part.
usize path_dirlen(const char* path, usize len);

// path_base returns a pointer to the last path element. E.g. "foo/bar/baz.x" => "baz.x"
// If the path is empty, returns "".
// If the path consists entirely of slashes, returns "/".
const char* path_base(const char* path);

ASSUME_NONNULL_END
