// SPDX-License-Identifier: Apache-2.0
#include "llvm/llvm.h"
#include "compiler.h"
#include "path.h"
#include "clang/Basic/Version.inc" // CLANG_VERSION_STRING

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h> // sleep
#include <err.h>
#include <libgen.h>

#include "../lib/musl/src/internal/version.h"
#define MUSL_VERSION_STR VERSION

typedef bool(*linkerfn_t)(int argc, char*const* argv, bool can_exit_early);

const char* coprogname;
const char* coexefile;
static char _coroot[PATH_MAX];
const char* coroot = _coroot;
u32 comaxproc = 1;

extern int main_build(int argc, char*const* argv); // build.c

static linkerfn_t nullable ld_impl(sys_t);
static const char* ld_impl_name(linkerfn_t nullable f);


static void usage(FILE* f) {
  // ld usage text
  linkerfn_t ldf = ld_impl(target_default()->sys);
  char host_ld[128];
  if (ldf) {
    snprintf(host_ld, sizeof(host_ld),
      "  ld [args ...]        Linker for host system (%s)\n", ld_impl_name(ldf));
  } else {
    host_ld[0] = 0;
  }

  fprintf(f,
    "Compis " CO_VERSION_STR ", your friendly neighborhood programming language\n"
    "Usage: %s <command> [args ...]\n"
    "Commands:\n"
    "  build [args ...]     Compis compiler\n"
    "  help, --help         Print help on stdout and exit\n"
    "  version, --version   Print version on stdout and exit\n"
    "Extra commands:\n"
    "  targets              List supported targets\n"
    "  cc [args ...]        C compiler (Clang)\n"
    "  as [args ...]        LLVM assembler (same as cc -cc1as)\n"
    "  ar [args ...]        Object archiver\n"
    "%s" // ld for host, if any
    "  ld-coff [args ...]   Linker for COFF\n"
    "  ld-elf [args ...]    Linker for ELF\n"
    "  ld-macho [args ...]  Linker for Mach-O\n"
    "  ld-wasm [args ...]   Linker for WebAssembly\n"
    "For help with a specific command:\n"
    "  %s <command> --help\n"
    "",
    coprogname,
    host_ld,
    coprogname);
}


void print_co_version() {
  printf("compis " CO_VERSION_STR);
  #ifdef CO_VERSION_GIT_STR
    printf(" (" CO_VERSION_GIT_STR ")");
  #endif
  const target_t* host = target_default();
  printf(
    " %s-%s"
    ", llvm " CLANG_VERSION_STRING
    ", musl " MUSL_VERSION_STR
    "\n",
    arch_name(host->arch), sys_name(host->sys));
}


static int ar_main(int argc, char*const* argv) {
  // TODO: accept --target triple (where we really only parse the os)
  const char* outfile = argv[1];
  char*const* filesv = &argv[2];
  u32 filesc = argc-2;
  char* errmsg = "?";
  CoLLVMArchiveKind arkind = llvm_sys_archive_kind(target_default()->sys);
  err_t err = llvm_write_archive(arkind, outfile, filesv, filesc, &errmsg);
  if (!err)
    return 0;
  warnx("%s", errmsg);
  LLVMDisposeMessage(errmsg);
  return 1;
}


static const char* ld_impl_name(linkerfn_t nullable f) {
  if (f == LLDLinkMachO) return "Mach-O";
  if (f == LLDLinkELF)   return "ELF";
  if (f == LLDLinkWasm)  return "WebAssembly";
  if (f == LLDLinkCOFF)  return "COFF";
  return "?";
}


static linkerfn_t nullable ld_impl(sys_t sys) {
  switch ((enum target_sys)sys) {
    case SYS_macos:
      return LLDLinkMachO;
    case SYS_linux:
      return LLDLinkELF;
    // case SYS_windows:
    //   return LLDLinkCOFF;
    // case SYS_wasi:
    // case SYS_wasm:
    //   return LLDLinkWasm;
    case SYS_none:
    case SYS_COUNT:
      return NULL;
  }
}


static int ld_main(int argc, char* argv[]) {
  linkerfn_t impl = safechecknotnull( ld_impl(target_default()->sys) );
  return !impl(argc, argv, true);
}


int main(int argc, char* argv[]) {
  coprogname = strrchr(argv[0], PATH_SEPARATOR);
  coprogname = coprogname ? coprogname + 1 : argv[0];
  coexefile = safechecknotnull(LLVMGetMainExecutable(argv[0]));
  safechecknotnull(dirname_r(coexefile, _coroot));
  relpath_init();

  // allow overriding coroot with env var
  const char* coroot_env = getenv("COROOT");
  if (coroot_env && *coroot_env)
    coroot = coroot_env;

  #if DEBUG
  if (str_endswith(coroot, "/out/debug"))
    coroot = path_join_m(memalloc_ctx(), coroot, "../..");
  #endif

  const char* exe_basename = strrchr(coexefile, PATH_SEPARATOR);
  exe_basename = exe_basename ? exe_basename + 1 : coexefile;

  bool is_multicall = (
    strcmp(coprogname, exe_basename) != 0 &&
    strcmp(coprogname, "compis") != 0 &&
    strcmp(coprogname, "co") != 0 );
  const char* cmd = is_multicall ? coprogname : argv[1] ? argv[1] : "";
  usize cmdlen = strlen(cmd);

  if (cmdlen == 0) {
    log("%s: missing command; try `%s help`", coprogname, coprogname);
    return 1;
  }

  err_t err = llvm_init();
  if (err)
    errx(1, "llvm_init: %s", err_str(err));

  #define ISCMD(s) (cmdlen == strlen(s) && memcmp(cmd, (s), cmdlen) == 0)

  // clang "cc" may spawn itself in a new process
  if (ISCMD("-cc1") || ISCMD("-cc1as"))
    return clang_main(argc, argv);

  if ISCMD("as") {
    argv[1] = "-cc1as";
    return clang_main(argc, argv);
  }

  // shave away "prog" from argv when not a multicall
  if (!is_multicall) {
    argc--;
    argv++;
  }

  comaxproc = sys_ncpu();

  // primary commands
  if ISCMD("build") return main_build(argc, argv);

  // secondary commands
  if ISCMD("targets") return print_supported_targets(), 0;

  // llvm-based commands
  if ISCMD("cc")       return clang_main(argc, argv);
  if ISCMD("ar")       return ar_main(argc, argv);
  if ISCMD("ld")       return ld_main(argc, argv);
  if ISCMD("ld-macho") return LLDLinkMachO(argc, argv, true) ? 0 : 1;
  if ISCMD("ld-elf")   return LLDLinkELF(argc, argv, true) ? 0 : 1;
  if ISCMD("ld-coff")  return LLDLinkCOFF(argc, argv, true) ? 0 : 1;
  if ISCMD("ld-wasm")  return LLDLinkWasm(argc, argv, true) ? 0 : 1;

  if (ISCMD("help") || ISCMD("--help") || ISCMD("-h")) {
    usage(stdout);
    return 0;
  }

  if (ISCMD("version") || ISCMD("--version")) {
    print_co_version();
    return 0;
  }

  log("%s: unknown command \"%s\"", coprogname, cmd);
  return 1;
}
