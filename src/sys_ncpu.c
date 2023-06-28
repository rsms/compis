// SPDX-License-Identifier: Apache-2.0
#include "colib.h"

#if defined(WIN32)
  #include <windows.h>
#else
  #include <errno.h>
  #if defined(__linux__)
    #include <sched.h>
  #elif defined(__APPLE__)
    #include <sys/sysctl.h>
  #endif
#endif


#if defined(WIN32)
#warning this implementation of sys_ncpu needs more testing

u32 sys_ncpu() {
  SYSTEM_INFO sysinfo;
  GetSystemInfo(&sysinfo);
  // maybe use GetLogicalProcessorInformation instead?
  return (u32)sysinfo.dwNumberOfProcessors;
}

#elif defined(__linux__)

u32 sys_ncpu() {
  u32 count = 0;
  unsigned long mask[1024];
  if UNLIKELY(sched_getaffinity(0, sizeof(mask), (void*)mask) != 0) {
    elog("sys_ncpu failure, errno=%d", errno);
    return 1;
  }
  for (usize i = 0; i < countof(mask); i++) {
    while (mask[i]) {
      if (mask[i] & 1)
        count++;
      mask[i] >>= 1;
    }
  }
  return count == 0 ? 1 : count;
}

#elif defined(__APPLE__)

u32 sys_ncpu() {
  // see *-macos/include/sys/sysctl.h
  i32 value;
  usize len = sizeof(value);
  if UNLIKELY(sysctlbyname("hw.activecpu", &value, &len, NULL, 0) != 0) {
    elog("sys_ncpu failure, errno=%d", errno);
    return 1;
  }
  if (value > 0)
    return (u32)value;
  return 1;
}

#elif defined(__posix__)
#warning this implementation of sys_ncpu needs more testing

u32 sys_ncpu() {
  int mib[4];
  mib[0] = CTL_HW;
  mib[1] = HW_AVAILCPU;
  int n;
  size_t len = sizeof(n);
  if (sysctl(mib, 2, &n, &len, NULL, 0) == 0 || n > 0)
    return (u32)n;
  // try HW_NCPU
  mib[1] = HW_NCPU;
  if (sysctl(mib, 2, &n, &len, NULL, 0) == 0 || n > 0)
    return (u32)n;
}

#endif
