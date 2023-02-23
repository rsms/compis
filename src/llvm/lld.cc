// lld api
// SPDX-License-Identifier: Apache-2.0
#include "llvm-includes.hh"
#include "llvmimpl.h"
#include <sstream>

#ifdef WIN32
  #define PATH_SEP "\\"
#else
  #define PATH_SEP "/"
#endif

using namespace llvm;


// lld api:          <llvm>/lld/include/lld/Common/Driver.h
// lld program main: <llvm>/lld/tools/lld/lld.cpp
// lld exe names:
//   ld.lld (Unix)
//   ld64.lld (macOS)
//   lld-link (Windows)
//   wasm-ld (WebAssembly)


// Sooooo lld's memory may become corrupt and requires a restart of the program.
// _lld_is_corrupt tracks this state. See lld::safeLldMain in lld/tools/lld/lld.cpp
static const char* _lld_is_corrupt = NULL;


static void _set_lld_is_corrupt(int errcode) {
  if (_lld_is_corrupt == NULL) {
    size_t bufcap = 128;
    char* buf = (char*)malloc(bufcap);
    snprintf(buf, bufcap, "lld crashed with exception code %d", errcode);
    _lld_is_corrupt = buf;
  }
}

// link functions' signature:
// bool link(llvm::ArrayRef<const char *> args, llvm::raw_ostream &stdoutOS,
//           llvm::raw_ostream &stderrOS, bool exitEarly, bool disableOutput);
typedef bool(*LinkFun)(
  llvm::ArrayRef<const char*> args,
  llvm::raw_ostream &stdoutOS,
  llvm::raw_ostream &stderrOS,
  bool exitEarly,
  bool disableOutput);


static LinkFun select_linkfn(Triple& triple, const char** cliname) {
  switch (triple.getObjectFormat()) {
    case Triple::COFF:  *cliname = "lld-link"; return lld::coff::link;
    case Triple::ELF:   *cliname = "ld.lld";   return lld::elf::link;
    case Triple::MachO: *cliname = "ld64.lld"; return lld::macho::link;
    case Triple::Wasm:  *cliname = "wasm-ld";  return lld::wasm::link;

    case Triple::GOFF:  // ?
    case Triple::XCOFF: // ?
    case Triple::SPIRV: // ?
    case Triple::DXContainer: // ?
    case Triple::UnknownObjectFormat:
      break;
  }
  return NULL;
}


static const char* triple_archname(const Triple& triple) {
  return (const char*)Triple::getArchTypeName(triple.getArch()).bytes_begin();
}


struct LinkerArgs {
  const CoLLVMLink&         options;
  Triple&                   triple;
  std::vector<const char*>& args;
  std::vector<std::string>& tmpstrings;
  char                      tmpbuf[PATH_MAX*2];

  const char* tmpstr(std::string&& s) {
    tmpstrings.emplace_back(s);
    return tmpstrings.back().c_str();
  }
  const char* tmpstr(const std::string& s) {
    tmpstrings.emplace_back(s);
    return tmpstrings.back().c_str();
  }

  void addargs(std::initializer_list<const char*> v) {
    for (auto arg : v)
      args.emplace_back(arg);
  }

  template<typename... T>
  void addarg(T&& ... args) { addargs({std::forward<T>(args)...}); }

  void addarg(std::string&& s) {      args.emplace_back(tmpstr(std::move(s))); }
  void addarg(const std::string& s) { args.emplace_back(tmpstr(s)); }
  void addarg(std::string s) {        args.emplace_back(tmpstr(s)); }
  void addarg(char* s) {              args.emplace_back(tmpstr(s)); }

  void addargf(const char* fmt, ...) ATTR_FORMAT(printf, 2, 3) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(tmpbuf, sizeof(tmpbuf), fmt, ap);
    addarg(tmpbuf);
    va_end(ap);
  }

  err_t add_lto_args();
  err_t add_coff_args();
  err_t add_elf_arg();
  err_t add_macho_args();
  err_t add_wasm_args();

  err_t unsupported_sys();
};


err_t LinkerArgs::unsupported_sys() {
  #if DEBUG
  auto osname = (const char*)Triple::getOSTypeName(triple.getOS()).bytes_begin();
  dlog("lld: unsupported system %s (%s)", osname, triple.str().c_str());
  #endif
  return ErrNotSupported;
}


err_t LinkerArgs::add_lto_args() {
  if (options.lto_level == 0)
    return 0;

  auto objformat = triple.getObjectFormat();

  if (objformat != Triple::COFF) {
    addarg(options.lto_level == 1 ? "--lto-O1" :
           options.lto_level == 2 ? "--lto-O2" :
                                    "--lto-O3");
    addarg("--no-lto-legacy-pass-manager");
    addarg("--thinlto-cache-policy=prune_after=24h");
  }

  if (*options.lto_cachedir) switch (objformat) {
    case Triple::COFF:
      addargf("/lldltocache:%s", options.lto_cachedir);
      addarg("/lldltocachepolicy:prune_after=24h");
      break;
    case Triple::MachO:
      addarg("-cache_path_lto", options.lto_cachedir);
      break;
    case Triple::ELF:
    case Triple::Wasm:
      addargf("--thinlto-cache-dir=%s", options.lto_cachedir);
      break;
    case Triple::GOFF:
    case Triple::XCOFF:
    case Triple::DXContainer:
    case Triple::SPIRV:
    case Triple::UnknownObjectFormat:
      break;
  }

  return 0;
}


err_t LinkerArgs::add_coff_args() {
  // flavor=lld-link
  // if (options.outfile)
  //   args.emplace_back(mktmpstr(std::string("/out:") + options.outfile));
  // note: "/machine:" seems similar to "-arch"
  dlog("TODO: %s", __FUNCTION__);
  return ErrNotSupported;
}


err_t LinkerArgs::add_elf_arg() {
  // flavor=ld.lld

  if (triple.getOS() != Triple::Linux)
    return unsupported_sys();

  addarg("--pie");
  addargf("--sysroot=%s", options.sysroot);
  addarg("-EL", "--build-id", "--eh-frame-hdr");

  // https://github.com/llvm/llvm-project/blob/llvmorg-15.0.7/lld/ELF/Driver.cpp#L131
  const char* target_emu = "";
  switch (triple.getArch()) {
    case Triple::ArchType::aarch64: target_emu = "aarch64linux"; break;
    case Triple::ArchType::arm:     target_emu = "armelf"; break;
    case Triple::ArchType::riscv32: target_emu = "elf32lriscv"; break;
    case Triple::ArchType::riscv64: target_emu = "elf64lriscv"; break;
    case Triple::ArchType::x86_64:  target_emu = "elf_x86_64"; break;
    case Triple::ArchType::x86:     target_emu = "elf_i386"; break;
    default:
      dlog("lld: unexpected arch %s", triple_archname(triple));
      return ErrNotSupported;
  }
  addarg("-m", target_emu);

  if (options.strip_dead)
    addarg("-s"); // Strip all symbols. Implies --strip-debug

  if (options.outfile)
    addarg("-o", options.outfile);

  addarg("-static");
  addargf("-L%s/lib", options.sysroot);
  addarg("-lc", "-lrt");

  addargf("%s" PATH_SEP "lib" PATH_SEP "crt1.o", options.sysroot);

  return 0;
}


err_t LinkerArgs::add_macho_args() {
  // flavor=ld64.lld

  // we only support macos (not IOS, TvOS or WatchOS)
  if (triple.getOS() != Triple::Darwin && triple.getOS() != Triple::MacOSX)
    return unsupported_sys();

  addarg("-pie");
  addarg("-demangle"); // demangle symbol names in diagnostics
  addarg("-adhoc_codesign");
  addarg("-syslibroot", options.sysroot);

  // LLD expects "arm64", not "aarch64", for apple platforms
  auto arch = triple.getArch();
  const char* arch_name;
  const char* macos_ver;
  if (arch == Triple::ArchType::aarch64) {
    arch_name = "arm64";
    macos_ver = "11.0.0";
  } else {
    arch_name = triple_archname(triple);
    macos_ver = "10.15.0";
  }

  // -platform_version <platform> <min_version> <sdk_version>
  addarg("-platform_version", "macos", macos_ver, macos_ver);
  addarg("-arch", arch_name);

  if (options.strip_dead)
    addarg("-dead_strip"); // remove unreferenced code and data

  if (options.outfile)
    addarg("-o", options.outfile);

  addargf("-L%s/lib", options.sysroot);
  addarg("-lc", "-lrt");
  return 0;
}


err_t LinkerArgs::add_wasm_args() {
  // flavor=wasm-ld
  dlog("TODO: %s", __FUNCTION__);
  addarg("--no-pie");
  return ErrNotSupported;
}


// build_args selects the linker function and adds args according to options and triple.
// This does not add options.infilev but it does add options.outfile (if not null)
// as that flag is linker-dependent.
// tmpstrings is a list of std::strings that should outlive the call to the returned
// LinkFun.
static err_t build_args(
  const CoLLVMLink&         options,
  Triple&                   triple,
  LinkFun*                  linkfn_out,
  std::vector<const char*>& args,
  std::vector<std::string>& tmpstrings)
{
  LinkerArgs linker_args{
    .options = options,
    .triple = triple,
    .args = args,
    .tmpstrings = tmpstrings,
  };

  // select link function
  const char* arg0 = "";
  *linkfn_out = select_linkfn(triple, &arg0);
  if (*linkfn_out == NULL)
    return ErrNotSupported;
  args.emplace_back(arg0);

  linker_args.add_lto_args();

  // build_args impl depending on linker implementation
  switch (triple.getObjectFormat()) {
    case Triple::COFF:
      return linker_args.add_coff_args();
    case Triple::ELF:
      return linker_args.add_elf_arg();
    case Triple::MachO:
      return linker_args.add_macho_args();
    case Triple::Wasm:
      return linker_args.add_wasm_args();
    case Triple::GOFF:
    case Triple::XCOFF:
    case Triple::DXContainer:
    case Triple::SPIRV:
    case Triple::UnknownObjectFormat:
      break;
  }
  dlog("unexpected object format");
  return ErrNotSupported;
}


// _link is a helper wrapper for calling the various object-specific linker functions
// It has been adapted from lld::safeLldMain in lld/tools/lld/lld.cpp
// Always sets errmsg
static err_t link_main(
  LinkFun linkf, llvm::ArrayRef<const char*> args, bool print_args)
{
  if (_lld_is_corrupt)
    return ErrMFault;

  if (print_args) {
    bool first = true;
    std::string s;
    for (auto& arg : args) {
      if (first) {
        first = false;
      } else {
        s += " ";
      }
      s += arg;
    }
    // Note: std::ostringstream with std::copy somehow adds an extra
    // empty item at the end.
    fprintf(stderr, "%s\n", s.c_str());
  }

  // stderr
  std::string errstr;
  raw_string_ostream errout(errstr);

  err_t err = 0;
  {
    // The crash recovery is here only to be able to recover from arbitrary
    // control flow when fatal() is called (through setjmp/longjmp or __try/__except).
    llvm::CrashRecoveryContext crc;
    const bool exitEarly = false;
    if (!crc.RunSafely([&]() {
      if (linkf(args, llvm::outs(), errout, exitEarly, false) == false)
        err = ErrInvalid; // TODO: better error code
    }))
    {
      _set_lld_is_corrupt(crc.RetCode);
    }
  }

  // Cleanup memory and reset everything back in pristine condition. This path
  // is only taken when LLD is in test, or when it is used as a library.
  llvm::CrashRecoveryContext crc;
  if (!crc.RunSafely([&]() { lld::CommonLinkerContext::destroy(); })) {
    // The memory is corrupted beyond any possible recovery
    _set_lld_is_corrupt(crc.RetCode);
  }

  std::string errs = errout.str();
  if (errs.size() > 0) {

    #if __APPLE__
      // split up over lines and ignore the following lines when linking
      // for Apple platforms:
      //   "ld64.lld: warning: /usr/lib/libSystem.dylib has version 10.15.0,\n"
      //   "which is newer than target minimum of 10.9.0"
      // which originates in checkCompatibility at llvm-src/lld/MachO/InputFiles.cpp
      std::istringstream iss(errs);
      const char* prefix1 = "ld64.lld: warning:";
      const char* middle1 = "which is newer than target minimum";
      for (std::string line; std::getline(iss, line); ) {
        if (line.find(prefix1) == 0 && line.find(middle1) != std::string::npos)
          continue;
        fwrite(line.data(), line.size(), 1, stderr);
        fputc('\n', stderr);
      }
    #else
      fwrite(errs.data(), errs.size(), 1, stderr);
    #endif

    if (_lld_is_corrupt)
      err = ErrMFault; // TODO: better error code
  }

  return err;
}


err_t llvm_link(const CoLLVMLink* optionsptr) {
  const CoLLVMLink& options = *optionsptr;
  Triple triple(Triple::normalize(options.target_triple));

  // arguments to linker and temporary string storage for them
  std::vector<const char*> args;
  std::vector<std::string> tmpstrings;

  // select linker function and build arguments
  LinkFun linkfn;
  err_t err = build_args(options, triple, &linkfn, args, tmpstrings);
  if UNLIKELY(err) {
    if (err == ErrNotSupported)
      log("linking %s not yet implemented", triple.str().c_str());
    return err;
  }

  // add input files
  for (u32 i = 0; i < options.infilec; i++)
    args.emplace_back(options.infilev[i]);

  // invoke linker
  return link_main(linkfn, args, options.print_lld_args);
}


// object-specific linker functions for exporting as C API:

bool LLDLinkCOFF(int argc, char*const*argv, bool can_exit_early) {
  std::vector<const char *> args(argv, argv + argc);
  return lld::coff::link(args, llvm::outs(), llvm::errs(), can_exit_early, false);
}

bool LLDLinkELF(int argc, char*const*argv, bool can_exit_early) {
  std::vector<const char *> args(argv, argv + argc);
  return lld::elf::link(args, llvm::outs(), llvm::errs(), can_exit_early, false);
}

bool LLDLinkMachO(int argc, char*const*argv, bool can_exit_early) {
  std::vector<const char *> args(argv, argv + argc);
  return lld::macho::link(args, llvm::outs(), llvm::errs(), can_exit_early, false);
}

bool LLDLinkWasm(int argc, char*const*argv, bool can_exit_early) {
  std::vector<const char *> args(argv, argv + argc);
  return lld::wasm::link(args, llvm::outs(), llvm::errs(), can_exit_early, false);
}

// // lld_link_macho links objects, archives and shared libraries together into a Mach-O executable.
// // If exitearly is true, this function calls exit(1) on error instead of returning false.
// // Always sets errmsg; on success it contains warning messages (if any.)
// // Caller must always call LLVMDisposeMessage on errmsg.
// // Returns true on success.
// bool lld_link_macho(int argc, const char** argv, char** errmsg) {
//   // Note: there's both lld::mach_o and lld::macho -- the latter is a newer linker
//   // which is work in progress (April 2021.)
//   // return _link(lld::macho::link, "ld64.lld", argc, argv); // WIP
//   std::vector<const char*> args(argv, argv + argc);
//   args.insert(args.begin(), "ld64.lld");
//   return _link(lld::mach_o::link, args, errmsg);
// }

// // lld_link_coff links objects, archives and shared libraries together into a COFF executable.
// // See lld_link_macho for details
// bool lld_link_coff(int argc, const char** argv, char** errmsg) {
//   std::vector<const char*> args(argv, argv + argc);
//   args.insert(args.begin(), "lld-link");
//   return _link(lld::coff::link, args, errmsg);
// }

// // lld_link_elf links objects, archives and shared libraries together into a ELF executable.
// // See lld_link_macho for details
// bool lld_link_elf(int argc, const char** argv, char** errmsg) {
//   std::vector<const char*> args(argv, argv + argc);
//   args.insert(args.begin(), "ld.lld");
//   return _link(lld::elf::link, args, errmsg);
// }

// // lld_link_wasm links objects, archives and shared libraries together into a WASM module.
// // See lld_link_macho for details
// bool lld_link_wasm(int argc, const char** argv, char** errmsg) {
//   std::vector<const char*> args(argv, argv + argc);
//   args.insert(args.begin(), "wasm-ld");
//   return _link(lld::wasm::link, args, errmsg);
// }
