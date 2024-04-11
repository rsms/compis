// compilation targets
// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "buf.h"
#ifdef __cplusplus
extern "C" {
#endif
ASSUME_NONNULL_BEGIN

// TARGET_FMT_BUFCAP: ideal buffer size fo target_fmt()
#define TARGET_FMT_BUFCAP 24

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

typedef enum syslib {
  SYSLIB_RT,     // librt
  SYSLIB_C,      // libc
  SYSLIB_CXX,    // libc++
  SYSLIB_CXXABI, // libc++abi
  SYSLIB_UNWIND, // libunwind
} syslib_t;

ASSUME_NONNULL_END

#define TARGET CO_PLUS_ONE
enum { SUPPORTED_TARGETS_COUNT = (0lu
  #include "targets.h"
) };
#undef TARGET

#undef ARCH
#define ARCH(name, ...) ARCH_##name,
enum target_arch {
  #include "targets.h"
};
#undef ARCH
#define ARCH CO_PLUS_ONE
enum { ARCH_COUNT = (0lu
  #include "targets.h"
) };
#undef ARCH

#undef SYS
#define SYS(name, ...) SYS_##name,
enum target_sys {
  #include "targets.h"
};
#undef SYS
#define SYS CO_PLUS_ONE
enum { SYS_COUNT = (0lu
  #include "targets.h"
) };
#undef SYS

#undef ARCH
#undef TARGET
ASSUME_NONNULL_BEGIN

extern const target_t supported_targets[];


// target = arch "-" sys ("." sysver)?
const target_t* target_default(); // host target (aka "native" target)

const target_t* nullable target_find(const char* target); // null if invalid

// target_id_match returns true if pattern matches a targets string,
// as returned by target_fmt.
bool target_str_match(const char* target_str, const char* pattern);

// target_find_matching populates 'targets' with targets which string matches pattern;
// a case-insensitive glob pattern.
// Returns the total number of matching targets, which may be larger than targets_cap.
u32 target_find_matching(
  const char* pattern, const target_t** targets, u32 targets_cap);

usize target_fmt(const target_t* t, char* buf, usize bufcap);
const char* arch_name(arch_t);
const char* sys_name(sys_t);
void print_supported_targets(); // prints with log()

// target_layers returns all possible coroot "layers" for target
// as absolute paths, rooted in "{coroot}/{basedir}/", or "{coroot}/" if basedir is "".
// The resulting array of c-strings is at filevp, the length is the return value.
// Returns 0 on memory allocation failure.
// Caller should free the returned array using target_layer_files_free when done.
//
// Layers returned are ordered from most specific to most generic:
//   arch and sysver: [arch-sys.sysver, arch-sys, any-sys.sysver, any-sys]
//   arch:            [arch-sys, any-sys]
//   sysver:          [any-sys.sysver, any-sys]
//   only sys:        [any-sys]
//   no arch or sys:  []
//
char** nullable target_layers(
  const target_t* t, memalloc_t ma, u32* filec_out, const char* basedir);
void target_layers_free(memalloc_t ma, char** layers, u32 len);

// target_linker_name returns the name of the lld linker for target (e.g. "ld.lld")
// Returns "" if there's no linker for the target.
const char* target_linker_name(const target_t* t);

bool target_has_syslib(const target_t*, syslib_t);

inline static bool target_is_wasm(const target_t* t) {
  return t->arch == ARCH_wasm32 || t->arch == ARCH_wasm64;
}

inline static bool target_is_riscv(const target_t* t) {
  return /*t->arch == ARCH_riscv32 ||*/ t->arch == ARCH_riscv64;
}

inline static bool target_is_arm(const target_t* t) {
  return t->arch == ARCH_arm;
}

inline static bool target_is_apple(const target_t* t) {
  return t->arch == SYS_macos;
}

void target_llvm_version(const target_t* t, char buf[16]);

// target_from_llvm_triple sets target to match llvm_triple
// by having LLVM parse llvm_triple.
// TARGET_LLVM_TRIPLE_IGN_UNKN_SYS: if set and the system/os of the triple
// is not supported, then sys is set to SYS_none instead of returning an error.
#define TARGET_LLVM_TRIPLE_IGN_UNKN_SYS (1u<<0)
err_t target_from_llvm_triple(target_t* target, const char* llvm_triple, u32 flags);


ASSUME_NONNULL_END
#ifdef __cplusplus
}
#endif
