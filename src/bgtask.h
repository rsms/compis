// background task status
// SPDX-License-Identifier: Apache-2.0
#pragma once
ASSUME_NONNULL_BEGIN

typedef struct {
  memalloc_t ma;
  u32        ntotal;     // total number of jobs (you can change this anytime)
  u32        n;          // number of jobs started
  u64        start_time; // nanotime
  int        flags;
  u32        prefixlen;
  char       linebuf[];
} bgtask_t;

// flags
#define BGTASK_FANCY   (1<<0) // always use ANSI terminal control
#define BGTASK_NOFANCY (1<<1) // never use ANSI terminal control

bgtask_t* bgtask_start(memalloc_t ma, const char* name, u32 ntotal, int flags);
void bgtask_end(bgtask_t* bgt);
void bgtask_setstatus(bgtask_t* bgt, const char* cstr);
void bgtask_setstatusf(bgtask_t* bgt, const char* fmt, ...) ATTR_FORMAT(printf,2,3);

ASSUME_NONNULL_END
