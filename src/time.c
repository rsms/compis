// SPDX-License-Identifier: Apache-2.0
#include "colib.h"

#include <time.h>
#include <sys/time.h>
#if defined __APPLE__
  #include <mach/mach_time.h>
#endif
#include <sys/stat.h>


#if defined(__APPLE__)
  // fraction to multiply a value in mach tick units with to convert it to nanoseconds
  static mach_timebase_info_data_t tbase;
  __attribute__((constructor)) static void time_init() {
    if (mach_timebase_info(&tbase) != KERN_SUCCESS)
      panic("mach_timebase_info");
  }
#endif


static unixtime_t timestamp_of_timespec(struct timespec ts) {
  assert(ts.tv_sec > -1); // caller should check before
  return (unixtime_t)ts.tv_sec*1000000llu + (unixtime_t)ts.tv_nsec/1000llu;
}


unixtime_t unixtime_of_stat_mtime(const struct stat* st) {
  if (st->st_mtime < 0)
    return 0llu;
  #if defined(__linux__) || defined(__wasi__)
    // "struct stat" has "struct timespec st_mtim"
    return timestamp_of_timespec(st->st_mtim);
  #elif defined(__APPLE__)
    #if !defined(_POSIX_C_SOURCE) || defined(_DARWIN_C_SOURCE)
      return timestamp_of_timespec(st->st_mtimespec);
    #else
      return (unixtime_t)st->st_mtime*1000000llu + (unixtime_t)st->st_mtimensec/1000llu;
    #endif
  #else
    return (unixtime_t)st->st_mtime * 1000000llu;
  #endif
}


unixtime_t unixtime_now() {
  #ifdef CLOCK_REALTIME
    struct timespec ts;
    if UNLIKELY(clock_gettime(CLOCK_REALTIME, &ts) != 0) {
      dlog("clock_gettime: %s", err_str(err_errno()));
      return 0;
    }
    return ts.tv_sec < 0 ? 0 : timestamp_of_timespec(ts);
  #else
    struct timeval tv;
    if UNLIKELY(gettimeofday(&tv, 0) != 0) {
      dlog("gettimeofday: %s", err_str(err_errno()));
      return 0;
    }
    return tv.tv_sec < 0 ? 0 :
           (unixtime_t)tv.tv_sec*1000000llu + (unixtime_t)tv.tv_usec;
  #endif
  return 0;
}


u64 nanotime() {
  #if defined(__APPLE__)
    u64 t = mach_absolute_time();
    return (t * tbase.numer) / tbase.denom;
  #elif defined(CLOCK_MONOTONIC)
    struct timespec ts;
    safecheckexpr(clock_gettime(CLOCK_MONOTONIC, &ts), 0);
    return ((u64)(ts.tv_sec) * 1000000000) + ts.tv_nsec;
  // TODO #elif (defined _MSC_VER && (defined _M_IX86 || defined _M_X64))
  //   QueryPerformanceCounter
  #elif !defined(CO_NO_LIBC)
    struct timeval tv;
    safecheckexpr(gettimeofday(&tv, nullptr), 0);
    return ((u64)(tv.tv_sec) * 1000000000) + ((u64)(tv.tv_usec) * 1000);
  #else
    #warning TODO CO_NO_LIBC nanotime
    return 0;
  #endif
}


usize fmtduration(char buf[25], u64 duration_ns) {
  // max value: "18446744073709551615.1ms\0"
  const char* unit = "ns";
  u64 d = duration_ns;
  u64 f = 0;
  if (duration_ns >= 1000000000) {
    f = d % 1000000000;
    d /= 1000000000;
    unit = "s\0";
  } else if (duration_ns >= 1000000) {
    f = d % 1000000;
    d /= 1000000;
    unit = "ms";
  } else if (duration_ns >= 1000) {
    d /= 1000;
    unit = "us\0";
  }
  usize i = sfmtu64(buf, d, 10);
  if (unit[0] != 'u' && unit[0] != 'n') {
    // one decimal for units larger than microseconds
    buf[i++] = '.';
    char buf2[20];
    UNUSED usize n = sfmtu64(buf2, f, 10);
    assert(n > 0);
    buf[i++] = buf2[0]; // TODO: round instead of effectively ceil
  }
  buf[i++] = unit[0];
  buf[i++] = unit[1];
  buf[i] = 0;
  return i;
}


u64 microsleep(u64 microseconds) {
  u64 sec = microseconds / 1000000;
  struct timespec request = {
    .tv_sec  = (long)sec,
    .tv_nsec = (microseconds - sec*1000000) * 1000,
  };
  struct timespec remaining = {0};
  if (nanosleep(&request, &remaining) != 0)
    return remaining.tv_sec*1000000 + remaining.tv_nsec/1000;
  return 0;
}

