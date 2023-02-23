// SPDX-License-Identifier: Apache-2.0
#include "colib.h"

#include <errno.h>
#if defined(__linux__)
  #include <sched.h>
#elif defined(__APPLE__)
  #include <sys/sysctl.h>
#endif


u32 sys_ncpu() {
  u32 count = 0;

  #if defined(__linux__)
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
  #elif defined(__APPLE__)
    // see *-macos/include/sys/sysctl.h
    u64 value;
    usize len = sizeof(value);
    if (sysctlbyname("hw.activecpu", &value, &len, NULL, 0) != 0)
      goto err;
    count = value > U32_MAX ? U32_MAX : (u32)value;
  #endif

  return count == 0 ? 1 : count;
err:
  log("sys_ncpu failure, errno=%d", errno);
  return 1;
}
