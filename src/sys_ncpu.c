// SPDX-License-Identifier: Apache-2.0
#include "colib.h"

#include <errno.h>
#if defined(__linux__)
  #include <sched.h>
#elif defined(__APPLE__)
  #include <sys/sysctl.h>
#endif


u32 sys_ncpu() {
  #if defined(__linux__)
    u32 count = 0;
    unsigned long mask[1024];
    if (sched_getaffinity(0, sizeof(mask), (void*)mask) != 0)
      goto err;
    for (usize i = 0; i < countof(mask); i++) {
      while (mask[i]) {
        if (mask[i] & 1)
          count++;
        mask[i] >>= 1;
      }
    }
    return count == 0 ? 1 : count;
  #elif defined(__APPLE__)
    // see *-macos/include/sys/sysctl.h
    i32 value;
    usize len = sizeof(value);
    if (sysctlbyname("hw.activecpu", &value, &len, NULL, 0) != 0)
      goto err;
    if (value > 0)
      return (u32)value;
    return 1;
  #endif

err:
  log("sys_ncpu failure, errno=%d", errno);
  return 1;
}
