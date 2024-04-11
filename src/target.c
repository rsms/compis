// SPDX-License-Identifier: Apache-2.0
#include "colib.h"
#include "compiler.h"
#include "path.h"
#include "llvm/llvm.h"
#include <string.h>
#include <fnmatch.h>


#define TARGET(ARCH, SYS, SYSVER, ...) \
  static_assert(TARGET_FMT_BUFCAP > sizeof(#ARCH "-" #SYS "." SYSVER), "");
#include "targets.h"
#undef TARGET


const target_t supported_targets[] = {
  #define TARGET(ARCH, SYS, SYSVER, INTSIZE, PTRSIZE, TRIPLE) { \
    .arch=ARCH_##ARCH, .sys=SYS_##SYS, .sysver=SYSVER, \
    .intsize=INTSIZE, .ptrsize=PTRSIZE, \
    .triple=TRIPLE, \
  },
  #include "targets.h"
  #undef TARGET
};

static const char* arch_strtab[] = {
  #undef ARCH
  #define ARCH(name, ...) #name,
  #include "targets.h"
  #undef ARCH
};

static const char* sys_strtab[] = {
  #undef SYS
  #define SYS(name, ...) #name,
  #include "targets.h"
  #undef SYS
};


static bool slice_eq_case(slice_t s, const char* cstr) {
  usize len = strlen(cstr);
  return s.len == len && strncasecmp(s.p, cstr, len) == 0;
}


void print_supported_targets() {
  char tmpbuf[TARGET_FMT_BUFCAP];
  for (usize i = 0; i < SUPPORTED_TARGETS_COUNT; i++) {
    target_fmt(&supported_targets[i], tmpbuf, sizeof(tmpbuf));
    log("%s", tmpbuf);
  }
}


const char* arch_name(arch_t a) {
  return arch_strtab[a * (arch_t)(a < ARCH_COUNT)];
}

const char* sys_name(sys_t s) {
  return sys_strtab[s * (sys_t)(s < SYS_COUNT)];
}


const target_t* target_default() {
  // these must cover everything defined in targets.h
  // discovered via: echo | clang --target=aarch64-unknown -dM -E - | sort

  arch_t arch;
  #if defined(__aarch64__)
    arch = ARCH_aarch64;
  #elif defined(__arm__)
    arch = ARCH_arm;
  #elif defined(__x86_64__)
    arch = ARCH_x86_64;
  #elif defined(__i386__)
    arch = ARCH_i386;
  #elif defined(__riscv) && __riscv_xlen == 64
    arch = ARCH_riscv64;
  #elif defined(__riscv)
    arch = ARCH_riscv32;
  #elif defined(__wasm64__)
    arch = ARCH_wasm64;
  #elif defined(__wasm32__)
    arch = ARCH_wasm32;
  #else
    #error unknown default target arch
  #endif

  sys_t sys;
  #if defined(__linux__)
    sys = SYS_linux;
  #elif defined(__APPLE__)
    sys = SYS_macos;
  #else
    #error unknown default target sys
  #endif

  for (usize i = 0; i < SUPPORTED_TARGETS_COUNT; i++) {
    const target_t* t = &supported_targets[i];
    if (t->arch == arch && t->sys == sys)
      return t;
  }
  panic("no default target");
  return &supported_targets[0];
}


// TODO: allow specifying target features and introduce a parse function
// to replace target_find.
// E.g. "armv6{a,zvl32b}-linux" for ARMv6 with atomic instruction and 32-bit vectors.
// err_t target_parse(target_t* result, const char* target_str);


const target_t* target_find(const char* target_str) {
  // arch-sys[.ver]
  const char* endp = target_str + strlen(target_str);
  if (endp == target_str)
    return target_default();
  const char* sysp = strchr(target_str, '-');
  if (!sysp) {
    dlog("invalid target \"%s\": missing system after architecture", target_str);
    return NULL;
  }
  sysp++;
  const char* dotp = strchr(sysp, '.');
  const char* sysend = dotp ? dotp : endp;
  slice_t arch = {.p=target_str, .len=(usize)(sysp - 1 - target_str)};
  slice_t sys = {.p=sysp, .len=(usize)(sysend - sysp)};
  slice_t sysver = {0};
  if (dotp) {
    sysver.p = dotp + 1;
    sysver.len = (usize)(endp - (dotp + 1));
  };
  // dlog("target_parse(\"%s\") => (%.*s,%.*s,%.*s)", target_str,
  //   (int)arch.len, arch.chars, (int)sys.len, sys.chars,
  //   (int)sysver.len, sysver.chars);

  #if DEBUG
  bool found_without_version = false;
  #endif

  for (usize i = 0; i < SUPPORTED_TARGETS_COUNT; i++) {
    const target_t* t = &supported_targets[i];
    if (slice_eq_case(arch, arch_strtab[t->arch]) &&
        slice_eq_case(sys, sys_strtab[t->sys]))
    {
      if (sysver.len == 0 || slice_eq_case(sysver, t->sysver))
        return t;

      #if DEBUG
      found_without_version = true;
      #endif
    }
  }

  #if DEBUG
    if (found_without_version) {
      dlog("unsupported target system version \"%.*s\" of target \"%.*s-%.*s\"",
        (int)sysver.len, sysver.chars,
        (int)arch.len, arch.chars, (int)sys.len, sys.chars);
    }
  #endif

  return NULL;
}


bool target_str_match(const char* target_str, const char* pattern) {
  return fnmatch(pattern, target_str, FNM_CASEFOLD) == 0;
}


u32 target_find_matching(
  const char* pattern, const target_t** targets, u32 targets_cap)
{
  char tmpbuf[TARGET_FMT_BUFCAP];
  u32 nmatches = 0;
  for (usize i = 0; i < SUPPORTED_TARGETS_COUNT; i++) {
    target_fmt(&supported_targets[i], tmpbuf, sizeof(tmpbuf));
    if (target_str_match(tmpbuf, pattern)) {
      if (nmatches < targets_cap)
        targets[nmatches] = &supported_targets[i];
      nmatches++;
    }
  }
  return nmatches;
}


usize target_fmt(const target_t* t, char* buf, usize bufcap) {
  int n = snprintf(buf, bufcap, "%s-%s", arch_strtab[t->arch], sys_strtab[t->sys]);
  if (t->sysver && *t->sysver) {
    int n2 = snprintf(buf + n, bufcap - (usize)n, ".%s", t->sysver);
    n += n2;
    if (n2 < 0)
      n = -1;
  }
  return n < 0 ? 0 : (usize)n;
}


char** nullable target_layers(
  const target_t* target, memalloc_t ma, u32* len_out, const char* basedir)
{
  // ENABLE_ANY_NONE: define to enable the universal "any-none" layer.
  // Currently this is unused, but it could be useful in the future.
  //#define ENABLE_ANY_NONE

  // Layers with ENABLE_ANY_NONE enabled:
  //   arch and sysver: [arch-sys.sysver, arch-sys, any-sys.sysver, any-sys, any-none]
  //   arch:            [arch-sys, any-sys, any-none]
  //   sysver:          [any-sys.sysver, any-sys, any-none]
  //   only sys:        [any-sys, any-none]
  //   no arch or sys:  [any-none]
  //
  // Layers with ENABLE_ANY_NONE disabled:
  //   arch and sysver: [arch-sys.sysver, arch-sys, any-sys.sysver, any-sys]
  //   arch:            [arch-sys, any-sys]
  //   sysver:          [any-sys.sysver, any-sys]
  //   only sys:        [any-sys]
  //   no arch or sys:  []

  target_t t = *target; // local copy which we can edit
  usize cap = (
    (   ( 1 + ((*t.sysver != 0) * 1) ) // sysver/no-sysver dimension
      * (1 + (t.arch != ARCH_any)) )   // arch/no-arch dimension
    #ifdef ENABLE_ANY_NONE
    + (t.sys != SYS_none)              // has extra "any-none" at end
    #else
    - (t.sys == SYS_none)              // for e.g. "wasm32-none"
    #endif
  );
  #if !defined(ENABLE_ANY_NONE)
    if (cap == 0) {
      *len_out = 0;
      static const char dummy;
      return (void*)&dummy;
    }
  #endif
  char** layers = mem_alloc(ma, sizeof(char*)*cap + PATH_MAX*cap).p;
  if (!layers)
    return NULL;

  // string storage follows the array of pointers
  layers[0] = (void*)layers + sizeof(char*)*cap;
  for (usize i = 1; i < cap; i++)
    layers[i] = layers[i - 1] + PATH_MAX;

  usize diroffs;
  if (*basedir && path_isabs(basedir)) {
    diroffs = strlen(basedir);
    safecheck(diroffs < PATH_MAX);
    memcpy(layers[0], basedir, diroffs);
  } else {
    // write "coroot/basedir" or "coroot" to layers
    diroffs = (usize)snprintf(
      layers[0], PATH_MAX, "%s" PATH_SEP_STR "%s", coroot, basedir);
    safecheck(diroffs < PATH_MAX - 1);
  }
  if (diroffs >= PATH_MAX - 1)
    diroffs = PATH_MAX - 2;
  usize strcap = PATH_MAX - diroffs; // remaining string storage capacity
  if (*basedir) {
    strcap--;
    layers[0][diroffs++] = PATH_SEPARATOR;
  }
  for (usize i = 1; i < cap; i++)
    memcpy(layers[i], layers[0], diroffs);

  // format layers, starting with the most specific one, which is one of:
  //   "arch-sys.sysver" or "arch-sys" where "arch" is "any" if arch==ARCH_any.
  if (target_fmt(&t, layers[0]+diroffs, strcap) >= strcap)
    goto overflow;
  usize len = 1;

  if (*t.sysver) {
    // "arch-sys"
    t.sysver = "";
    assert(cap > len);
    assertf(t.sys != SYS_none, "none-system target with sysver makes no sense");
    if (target_fmt(&t, layers[len++]+diroffs, strcap) >= strcap)
      goto overflow;
  }

  if (
    t.arch != ARCH_any
    #if !defined(ENABLE_ANY_NONE)
      && t.sys != SYS_none
    #endif
  ) {
    // "any-sys.sysver" or "any-sys"
    t.arch = ARCH_any;
    t.sysver = target->sysver;
    assert(cap > len);
    if (target_fmt(&t, layers[len++]+diroffs, strcap) >= strcap)
      goto overflow;

    if (*t.sysver) {
      // "any-sys"
      t.sysver = "";
      assert(cap > len);
      if (target_fmt(&t, layers[len++]+diroffs, strcap) >= strcap)
        goto overflow;
    }
  }

  #ifdef ENABLE_ANY_NONE
    if (t.sys != SYS_none) {
      t.sys = SYS_none;
      assert(cap > len);
      if (target_fmt(&t, layers[len++]+diroffs, strcap) >= strcap)
        goto overflow;
    }
  #endif

  assertf(len == cap, "len(%zu) == cap(%zu)", len, cap);
  *len_out = (u32)len;
  return layers;

overflow:
  vlog("%s: path too long: %.*s", __FUNCTION__, (int)diroffs, layers[0]);
  target_layers_free(ma, layers, cap);
  return NULL;

  #undef ENABLE_ANY_NONE
}


void target_layers_free(memalloc_t ma, char** layers, u32 len) {
  mem_freex(ma, MEM(layers, sizeof(char*)*len + PATH_MAX*len));
}


const char* target_linker_name(const target_t* t) {
  switch ((enum target_sys)t->sys) {
    case SYS_linux: return "ld.lld";
    case SYS_macos: return "ld64.lld";
    case SYS_wasi:  return "wasm-ld";
    case SYS_win32: return "lld-link";
    case SYS_none:
      if (t->arch == ARCH_wasm32 || t->arch == ARCH_wasm64)
        return "wasm-ld";
      return "";
  }
  safefail("sys %u", t->sys);
  return "";
}


bool target_has_syslib(const target_t* t, syslib_t syslib) {
  switch ((enum syslib)syslib) {

  case SYSLIB_RT:
    if (target_is_wasm(t)) {
      // available for both wasmX-none and wasm-wasi
      return t->sys == SYS_none || t->sys == SYS_wasi;
    }
    return t->sys != SYS_none && t->sys != SYS_win32; // TODO win32

  case SYSLIB_C:
    return t->sys != SYS_none && t->sys != SYS_win32; // TODO win32

  case SYSLIB_CXX:
  case SYSLIB_CXXABI:
  case SYSLIB_UNWIND:
    return t->sys != SYS_none;
  }
  safefail("%s %d", __FUNCTION__, syslib);
  return false;
}


void target_llvm_version(const target_t* t, char buf[16]) {
  if (t->sys != SYS_macos) {
    buf[0] = 0;
    return;
  }
  if (streq(t->sysver, "10")) {
    memcpy(buf, "10.15.0", strlen("10.15.0") + 1);
  } else {
    usize i = strlen(t->sysver);
    safecheck(i < 16);
    memcpy(buf, t->sysver, i);
    snprintf(&buf[i], 16-i, ".0.0");
  }
}


err_t target_from_llvm_triple(target_t* target, const char* llvm_triple, u32 flags) {
  memset(target, 0, sizeof(*target));

  CoLLVMTargetInfo tinfo = {0};
  llvm_triple_info(llvm_triple, &tinfo);

  if (tinfo.arch_type == CoLLVMArch_unknown)
    return ErrInvalid;

  target->intsize = tinfo.ptr_size;
  target->ptrsize = tinfo.ptr_size;
  target->bigendian = !tinfo.is_little_endian;
  target->triple = strdup(llvm_triple); // FIXME
  target->sysver = ""; // FIXME

  switch (tinfo.arch_type) {
    case CoLLVMArch_arm:     target->arch = ARCH_arm; break;
    case CoLLVMArch_aarch64: target->arch = ARCH_aarch64; break;
    case CoLLVMArch_x86:     target->arch = ARCH_i386; break;
    case CoLLVMArch_x86_64:  target->arch = ARCH_x86_64; break;
    case CoLLVMArch_riscv64: target->arch = ARCH_riscv64; break;
    case CoLLVMArch_wasm32:  target->arch = ARCH_wasm32; break;
    case CoLLVMArch_wasm64:  target->arch = ARCH_wasm64; break;
    default: return ErrNotSupported;
  }

  switch (tinfo.os_type) {
    case CoLLVMOS_unknown: target->sys = SYS_none; break;
    case CoLLVMOS_Darwin:
    case CoLLVMOS_MacOSX:  target->sys = SYS_macos; break;
    case CoLLVMOS_Linux:   target->sys = SYS_linux; break;
    case CoLLVMOS_Win32:   target->sys = SYS_win32; break;
    case CoLLVMOS_WASI:    target->sys = SYS_wasi; break;
    default:
      if (flags & TARGET_LLVM_TRIPLE_IGN_UNKN_SYS) {
        target->sys = SYS_none;
      } else {
        return ErrNotSupported;
      }
  }

  return 0;
}
