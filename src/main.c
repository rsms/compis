#include "llvm/llvm.h"
#include <stdio.h>
#include <string.h> // strcmp
#include <err.h>

static const char* prog; // main program name
CoLLVMOS host_os;

extern int clang_main(int argc, const char** argv); // llvm/driver.cc
extern int build_main(int argc, const char** argv); // build.c


// static CoLLVMOS OSTypeParse(const char* name) {
//   if (strcmp(name, "darwin") == 0)  return OSDarwin;
//   if (strcmp(name, "freebsd") == 0) return OSFreeBSD;
//   if (strcmp(name, "ios") == 0)     return OSIOS;
//   if (strcmp(name, "linux") == 0)   return OSLinux;
//   if (strcmp(name, "macosx") == 0)  return OSMacOSX;
//   if (strcmp(name, "openbsd") == 0) return OSOpenBSD;
//   if (strcmp(name, "win32") == 0)   return OSWin32;
//   return OSUnknown;
// }

static void usage(FILE* f) {
  const char* host_os_typename = CoLLVMOS_name(host_os);
  fprintf(f,
    "usage: %s <command> [args ...]\n"
    "commands:\n"
    "  cc [args ...]        Clang\n"
    "  as [args ...]        LLVM assembler (same as cc -cc1as)\n"
    "  ar [args ...]        Create object archive\n"
    "  ld [args ...]        Linker for host system (%s)\n"
    "  ld-coff [args ...]   Linker for COFF\n"
    "  ld-elf [args ...]    Linker for ELF\n"
    "  ld-macho [args ...]  Linker for Mach-O\n"
    "  ld-wasm [args ...]   Linker for WebAssembly\n"
    "",
    prog,
    host_os_typename);
}

static int ar_main(int argc, const char** argv) {
  // TODO: accept --target triple (where we really only parse the os)

  // const char* osname = (strlen(argv[0]) > 2) ? &argv[0][3] : "";
  // CoLLVMOS os = OSDarwin; // TODO: ifdef ... select host
  // if (strlen(osname) > 0) {
  //   os = OSTypeParse(osname);
  //   if (os == OSUnknown) {
  //     fprintf(stderr,
  //       "%s: unknown archiver %s;"
  //       " expected one of -ar, -ar.darwin, -ar.freebsd, -ar.ios, -ar.linux,"
  //       " -ar.macosx, -ar.openbsd, -ar.win32\n",
  //       parentprog, argv[0]);
  //     return 1;
  //   }
  // }
  // if (argc < 3) {
  //   log("usage: %s %s <archive> <file> ...", parentprog, argv[0]);
  //   return 1;
  // }

  const char*  archivefile = argv[1];
  const char** filesv = &argv[2];
  u32          filesc = argc-2;
  CoLLVMOS     os = host_os;
  char* errmsg = "?";

  bool ok = llvm_write_archive(archivefile, filesv, filesc, os, &errmsg);
  if (!ok) {
    warnx("ld: %s", errmsg);
    LLVMDisposeMessage(errmsg);
    return 1;
  }
  return 0;
}


static int ld_main(int argc, const char** argv) {
  switch (host_os) {
    case CoLLVMOS_Darwin:
    case CoLLVMOS_MacOSX:
    case CoLLVMOS_IOS:
    case CoLLVMOS_TvOS:
    case CoLLVMOS_WatchOS:
      return LLDLinkMachO(argc, argv, true) ? 0 : 1;
    case CoLLVMOS_Win32:
      return LLDLinkCOFF(argc, argv, true) ? 0 : 1;
    case CoLLVMOS_WASI:
    case CoLLVMOS_Emscripten:
      return LLDLinkWasm(argc, argv, true) ? 0 : 1;
    // assume the rest uses ELF (this is probably not correct)
    case CoLLVMOS_Ananas:
    case CoLLVMOS_CloudABI:
    case CoLLVMOS_DragonFly:
    case CoLLVMOS_FreeBSD:
    case CoLLVMOS_Fuchsia:
    case CoLLVMOS_KFreeBSD:
    case CoLLVMOS_Linux:
    case CoLLVMOS_Lv2:
    case CoLLVMOS_NetBSD:
    case CoLLVMOS_OpenBSD:
    case CoLLVMOS_Solaris:
    case CoLLVMOS_Haiku:
    case CoLLVMOS_Minix:
    case CoLLVMOS_RTEMS:
    case CoLLVMOS_NaCl:
    case CoLLVMOS_AIX:
    case CoLLVMOS_CUDA:
    case CoLLVMOS_NVCL:
    case CoLLVMOS_AMDHSA:
    case CoLLVMOS_PS4:
    case CoLLVMOS_ELFIAMCU:
    case CoLLVMOS_Mesa3D:
    case CoLLVMOS_Contiki:
    case CoLLVMOS_AMDPAL:
    case CoLLVMOS_HermitCore:
    case CoLLVMOS_Hurd:
      return LLDLinkELF(argc, argv, true) ? 0 : 1;
    default:
      log("%s ld: unsupported host OS %s", prog, CoLLVMOS_name(host_os));
      return 1;
  }
}


int main(int argc, const char** argv) {
  prog = argv[0];

  const char* progname = strrchr(prog, '/');
  progname = progname ? progname + 1 : prog;
  bool is_multicall = strcmp(progname, "c0") != 0;
  const char* cmd = is_multicall ? progname : argv[1] ? argv[1] : "";
  usize cmdlen = strlen(cmd);

  err_t err = llvm_init();
  if (err)
    errx(1, "llvm_init: %s", err_str(err));

  host_os = LLVMGetHostOS();

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

  if ISCMD("build") return build_main(argc, argv);

  // clang-derived commands
  if ISCMD("cc")       return clang_main(argc, argv);
  if ISCMD("ar")       return ar_main(argc, argv);
  if ISCMD("ld")       return ld_main(argc, argv);
  if ISCMD("ld-macho") return LLDLinkMachO(argc, argv, true) ? 0 : 1;
  if ISCMD("ld-elf")   return LLDLinkELF(argc, argv, true) ? 0 : 1;
  if ISCMD("ld-coff")  return LLDLinkCOFF(argc, argv, true) ? 0 : 1;
  if ISCMD("ld-wasm")  return LLDLinkWasm(argc, argv, true) ? 0 : 1;

  if (cmdlen == 0) {
    log("%s: missing command", prog);
    return 1;
  }

  if (strstr(cmd, "-h") || strstr(cmd, "help")) {
    usage(stdout);
    return 0;
  }

  log("%s: unknown command \"%s\"", prog, cmd);
  return 1;
}
