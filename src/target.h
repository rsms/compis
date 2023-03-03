// compilation targets
// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "buf.h"
#ifdef __cplusplus
extern "C" {
#endif
ASSUME_NONNULL_BEGIN

typedef u8 arch_t; // enum arch
typedef u8 sys_t; // enum sys
typedef struct {
  arch_t      arch;
  sys_t       sys;
  const char* sysver;    // "" = no version
  u32         intsize;   // byte size of integer register, for "int" and "uint" types
  u32         ptrsize;   // byte size of pointer, e.g. 8 for i64
  bool        bigendian;
  const char* triple;    // for LLVM, e.g. x86_64-apple-darwin19, aarch64-linux-musl
} target_t;

typedef struct {
  arch_t      arch;
  sys_t       sys;
  const char* sysver;
} targetdesc_t;

ASSUME_NONNULL_END
#define TARGET CO_PLUS_ONE
enum { SUPPORTED_TARGETS_COUNT = (0lu
  #include "targets.h"
) };
#undef TARGET
#undef ARCH
#define ARCH(name, ...) ARCH_##name,
enum target_arch {
  ARCH_unknown,
  #include "targets.h"
  ARCH_COUNT
};
#undef ARCH
#undef SYS
#define SYS(name, ...) SYS_##name,
enum target_sys {
  SYS_none,
  #include "targets.h"
  SYS_COUNT
};
#undef SYS
#undef ARCH
#undef TARGET
ASSUME_NONNULL_BEGIN

extern const target_t supported_targets[];


// target = arch "-" sys ("." sysver)?
const target_t* target_default(); // host target (aka "native" target)
const target_t* nullable target_find(const char* target); // null if invalid
usize target_fmt(const target_t* t, char* buf, usize bufcap);
const char* arch_name(arch_t);
const char* sys_name(sys_t);
void print_supported_targets(); // prints with log()

typedef err_t (*target_str_visitor_t)(const char* s, void* ctx);

// target_visit_sysinc_dirs calls visitor for each possible dir for target,
// rooted in "{coroot}/{basedir}/", or if basedir is "", just "{coroot}/".
// Returns the first error returned from visitor.
err_t target_visit_dirs(target_t*, const char* basedir, target_str_visitor_t, void* ctx);


ASSUME_NONNULL_END
#ifdef __cplusplus
}
#endif
