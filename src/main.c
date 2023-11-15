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
#include <errno.h>
#include <libgen.h>

#include "../lib/musl/src/internal/version.h"
#define MUSL_VERSION_STR VERSION

#define COCACHE_DEFAULT ".cache/compis"

typedef bool(*linkerfn_t)(int argc, char*const* argv, bool can_exit_early);

const char* coprogname;
const char* coexefile;
const char* coroot;
const char* cocachedir;
const char*const* copath;
u8 coverbose = 0;
u32 comaxproc = 1;

// externally-implemented tools
int main_build(int argc, char*const* argv); // build.c
int cc_main(int argc, char* argv[], bool iscxx); // cc.c
int llvm_ar_main(int argc, char **argv); // llvm/llvm-ar.cc
int llvm_nm_main(int argc, char **argv); // llvm/llvm-nm.cc
int llvm_link_main(int argc, char **argv); // llvm/llvm-link.cc
int llvm_llc_main(int argc, char **argv); // llvm/llvm-llc.cc

static linkerfn_t nullable ld_impl(const target_t* t);
static const char* ld_impl_name(linkerfn_t nullable f);


static int usage(FILE* f) {
  // ld usage text
  linkerfn_t ldf = ld_impl(target_default());
  char host_ld[128];
  if (ldf) {
    snprintf(host_ld, sizeof(host_ld),
      "  ld        %s linker (host)\n", ld_impl_name(ldf));
  } else {
    host_ld[0] = 0;
  }

  fprintf(f,
    "Usage: %s <command> [args ...]\n"
    "Commands:\n"
    "  build     Build a package\n"
    "\n"
    "  ar        Archiver\n"
    "  cc        C compiler (clang)\n"
    "  c++       C++ compiler (clang++)\n"
    "  ranlib    Archive index generator\n"
    "  nm        Symbol table dumper\n"
    "\n"
    "%s" // ld for host, if any
    "  ld.lld    ELF linker\n"
    "  ld64.lld  Mach-O linker\n"
    "  lld-link  COFF linker\n"
    "  wasm-ld   WebAssembly linker\n"
    "\n"
    "  help      Print help on stdout and exit\n"
    "  targets   List supported targets\n"
    "  version   Print version on stdout and exit\n"
    "\n"
    "For help with a specific command:\n"
    "  %s <command> --help\n"
    "\n"
    "Environment variables:\n"
    "  COROOT    Bundled resources. Defaults to executable directory\n"
    "  COCACHE   Build cache. Defaults to ~/" COCACHE_DEFAULT "\n"
    "  COMAXPROC Parallelism limit. Defaults to number of CPUs (%u)\n"
    "\n",
    coprogname,
    host_ld,
    coprogname,
    sys_ncpu());
  return 0;
}


void print_co_version() {
  printf(
    "compis " CO_VERSION_STR " ("
    #ifdef CO_DEVBUILD
      "dev "
    #endif
    #if defined(CO_VERSION_GIT) && !defined(CO_DISTRIBUTION)
      "src=" CO_STRX(CO_VERSION_GIT) " "
    #endif
    "llvm=" CLANG_VERSION_STRING " "
    "musl=" MUSL_VERSION_STR
    ")\n");
}


static const char* ld_impl_name(linkerfn_t nullable f) {
  if (f == LLDLinkMachO) return "Mach-O";
  if (f == LLDLinkELF)   return "ELF";
  if (f == LLDLinkWasm)  return "WebAssembly";
  if (f == LLDLinkCOFF)  return "COFF";
  return "?";
}


static linkerfn_t nullable ld_impl(const target_t* t) {
  switch ((enum target_sys)t->sys) {
    case SYS_macos:
      return LLDLinkMachO;
    case SYS_linux:
      return LLDLinkELF;
    case SYS_win32:
      return LLDLinkCOFF;
    case SYS_wasi:
      return LLDLinkWasm;
    case SYS_none:
      if (t->arch == ARCH_wasm32 || t->arch == ARCH_wasm64)
        return LLDLinkWasm;
      break;
  }
  return NULL;
}


static int ld_main(int argc, char* argv[]) {
  linkerfn_t impl = safechecknotnull( ld_impl(target_default()) );
  return !impl(argc, argv, true);
}


static void coroot_init(memalloc_t ma) {
  const char* envvar = getenv("COROOT");
  if (envvar && *envvar) {
    coroot = path_abs(envvar).p;
  } else {
    coroot = path_dir(coexefile).p;
    #if !defined(CO_DISTRIBUTION)
      if (strstr(coroot, "/out/opt-") || strstr(coroot, "/out/debug-"))
        coroot = path_join(coroot, "../../lib").p;
    #endif
  }
  safecheck(coroot != NULL);
  char* probe = path_join_alloca(coroot, "co/coprelude.h");
  if (!fs_isfile(probe))
    warnx("warning: invalid COROOT '%s' (compiling may not work)", coroot);
}


static void copath_init(memalloc_t ma) {
  const char* s = getenv("COPATH");
  if (s && *s) {
    copath = (const char*const*)path_parselist(ma, s);
    safecheckf(copath, "out of memory");
  } else {
    static const char* copath_default[] = {".", NULL};
    copath = copath_default;
  }
}


static void cocachedir_init(memalloc_t ma) {
  const char* envvar = getenv("COCACHE");
  if (envvar && *envvar) {
    cocachedir = path_abs(envvar).p;
  } else {
    cocachedir = path_join(sys_homedir(), COCACHE_DEFAULT "/" CO_VERSION_STR).p;
  }
  safecheck(cocachedir != NULL);
}


static void comaxproc_init() {
  const char* envvar = getenv("COMAXPROC");
  if (envvar && *envvar) {
    char* end;
    unsigned long n = strtoul(envvar, &end, 10);
    if (n == ULONG_MAX || n > U32_MAX || *end || (n == 0 && errno))
      errx(1, "invalid value: COMAXPROC=%s", envvar);
    if (n > 0) {
      comaxproc = (u32)n;
      return;
    }
  }
  comaxproc = sys_ncpu();
  assert(comaxproc > 0);
}


int main(int argc, char* argv[]) {
  coprogname = strrchr(argv[0], PATH_SEPARATOR);
  coprogname = coprogname ? coprogname + 1 : argv[0];
  coexefile = safechecknotnull(LLVMGetMainExecutable(argv[0]));

  const char* exe_basename = strrchr(coexefile, PATH_SEPARATOR);
  exe_basename = exe_basename ? exe_basename + 1 : coexefile;

  bool is_multicall = (
    !streq(coprogname, exe_basename) &&
    !string_startswith(coprogname, "compis") &&
    !string_startswith(coprogname, "co") );
  const char* cmd = is_multicall ? coprogname : argv[1] ? argv[1] : "";

  if (*cmd == 0) {
    usage(stdout);
    elog("%s: missing command; try `%s help`", coprogname, coprogname);
    return 1;
  }

  #define IS(...)  __VARG_DISP(IS,__VA_ARGS__)
  #define IS1(name)  (streq(cmd, (name)))
  #define IS2(a,b)   (streq(cmd, (a)) || streq(cmd, (b)))
  #define IS3(a,b,c) (streq(cmd, (a)) || streq(cmd, (b)) || streq(cmd, (c)))

  // clang "cc" may spawn itself in a new process
  if IS("-cc1", "-cc1as")
    return clang_main(argc, argv);

  // shave away "prog" from argv when not a multicall
  if (!is_multicall) {
    argc--;
    argv++;
  }

  // commands that do not touch any compis code (no need for compis init)
  if IS("ld.lld")       return LLDLinkELF(argc, argv, true) ? 0 : 1;
  if IS("ld64.lld")     return LLDLinkMachO(argc, argv, true) ? 0 : 1;
  if IS("lld-link")     return LLDLinkCOFF(argc, argv, true) ? 0 : 1;
  if IS("wasm-ld")      return LLDLinkWasm(argc, argv, true) ? 0 : 1;
  if IS("ar", "ranlib") return llvm_ar_main(argc, argv); // adds  ~70kB to LTO build
  if IS("nm")           return llvm_nm_main(argc, argv); // adds ~250kB to LTO build

  // initialize global state
  memalloc_t ma = memalloc_ctx();
  comaxproc_init();
  relpath_init();
  tmpbuf_init(ma);
  sym_init(ma);
  typeid_init(ma);
  universe_init();
  coroot_init(ma);
  copath_init(ma);
  cocachedir_init(ma);
  err_t err = llvm_init();
  if (err) errx(1, "llvm_init: %s", err_str(err));

  // command dispatch
  if IS("build")                return main_build(argc, argv);
  if IS("cc", "clang")          return cc_main(argc, argv, /*iscxx*/false);
  if IS("c++", "clang++")       return cc_main(argc, argv, /*iscxx*/true);
  if IS("ld")                   return ld_main(argc, argv);
  if IS("targets")              return print_supported_targets(), 0;
  if IS("version", "--version") return print_co_version(), 0;
  if IS("help", "--help", "-h") return usage(stdout);

  elog("%s: unknown command \"%s\"", coprogname, cmd);
  return 1;
}
