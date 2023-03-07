// SPDX-License-Identifier: Apache-2.0
#include "colib.h"
#include "path.h"

#include <stdlib.h>
#include <unistd.h>
#include <pwd.h>
#include <err.h>


const char* sys_homedir() {
  static char homedir[PATH_MAX] = {0};
  if (*homedir)
    return homedir;

  usize bufsize = sysconf(_SC_GETPW_R_SIZE_MAX);
  if (bufsize == (usize)-1)
    bufsize = 16384;

  char* buf = alloca(bufsize);
  if (buf) {
    struct passwd pwd;
    struct passwd* result;
    getpwuid_r(getuid(), &pwd, buf, bufsize, &result);
    if (result) {
      memcpy(homedir, pwd.pw_dir, strlen(pwd.pw_dir) + 1);
      return homedir;
    }
    // note: getpwuid_r returns 0 if the user getuid() was not found (we don't care)
    warnx("sys_homedir/getpwuid_r");
  }

  // try HOME in env
  const char* home = getenv("HOME");
  if (home) {
    memcpy(homedir, home, strlen(home) + 1);
  } else {
    // last resort
    #if defined(WIN32)
      memcpy(homedir, "C:\\", strlen("C:\\") + 1);
    #else
      homedir[0] = PATH_SEPARATOR;
      homedir[1] = 0;
    #endif
  }

  return homedir;
}
