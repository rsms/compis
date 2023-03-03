// SPDX-License-Identifier: Apache-2.0
#include "colib.h"
#include "compiler.h"
#include <string.h>


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
  "unknown",
  #undef ARCH
  #define ARCH(name, ...) #name,
  #include "targets.h"
  #undef ARCH
};

static const char* sys_strtab[] = {
  "unknown",
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
  char tmpbuf[64];
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


err_t target_visit_dirs(
  target_t* t, const char* basedir, target_str_visitor_t visitor, void* ctx)
{
  char path[PATH_MAX];

  usize offs = (usize)snprintf(path, sizeof(path), "%s/%s", coroot, basedir);
  safecheck(offs < sizeof(path) - 1);
  if (offs >= sizeof(path) - 1)
    offs = sizeof(path) - 2;
  usize pathcap = sizeof(path) - offs;
  if (*basedir) {
    pathcap--;
    path[offs++] = '/';
  }

  err_t err = 0;

  target_fmt(t, &path[offs], pathcap);
  if (( err = visitor(path, ctx) ))
    goto end;

  if (*t->sysver) {
    snprintf(&path[offs], pathcap, "any-%s.%s", sys_name(t->sys), t->sysver);
    if (( err = visitor(path, ctx) ))
      goto end;
    snprintf(&path[offs], pathcap, "%s-%s", arch_name(t->arch), sys_name(t->sys));
    if (( err = visitor(path, ctx) ))
      goto end;
  }

  snprintf(&path[offs], pathcap, "any-%s", sys_name(t->sys));
  err = visitor(path, ctx);

end:
  return err;
}
