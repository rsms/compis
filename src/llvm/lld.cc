// lld api
// SPDX-License-Identifier: Apache-2.0
#include "llvm-includes.hh"
#include "llvmimpl.h"
#include <sstream>

// DEBUG_LLD_INVOCATION: define to print arguments used for linker invocations to stderr
#define DEBUG_LLD_INVOCATION

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


// build_args selects the linker function and adds args according to options and triple.
// This does not add options.infilev but it does add options.outfile (if not null) as that
// flag is linker-dependent.
// tmpstrings is a list of std::strings that should outlive the call to the returned LinkFun.
static err_t build_args(
  const CoLLVMLink&         options,
  Triple&                   triple,
  LinkFun*                  linkfn_out,
  std::vector<const char*>& args,
  std::vector<std::string>& tmpstrings)
{
  auto mktmpstr = [&](std::string&& s) {
    tmpstrings.emplace_back(s);
    return tmpstrings.back().c_str();
  };

  // select link function
  const char* arg0 = "";
  *linkfn_out = select_linkfn(triple, &arg0);
  if (*linkfn_out == NULL)
    return ErrNotSupported;
  args.emplace_back(arg0);

  // common arguments
  // (See lld flavors' respective CLI help output, e.g. deps/llvm/bin/lld -flavor ld.lld -help)
  if (triple.getObjectFormat() == Triple::COFF) {
    // Windows (flavor=lld-link, flagstyle="/flag")
    // TODO consider adding "/machine:" which seems similar to "-arch"
    if (options.outfile)
      args.emplace_back(mktmpstr(std::string("/out:") + options.outfile));
  } else {
    // Rest of the world (flavor=!lld-link, flagstyle="-flag")
    auto archname = (const char*)Triple::getArchTypeName(triple.getArch()).bytes_begin();
    switch (triple.getOS()) {
      case Triple::Darwin:
      case Triple::MacOSX:
      case Triple::IOS:
      case Triple::TvOS:
      case Triple::WatchOS:
        if (triple.getArch() == Triple::ArchType::aarch64) {
          // LLD expects "arm64", not "aarch64", for these platforms
          archname = "arm64";
        }
        break;
      default:
        break;
    }
    args.emplace_back("-arch");
    args.emplace_back(archname);
    if (options.outfile) {
      args.emplace_back("-o");
      args.emplace_back(options.outfile);
    }
  }

  // LTO
  if (options.lto_level > 0) {
    args.emplace_back(
      options.lto_level == 1 ? "--lto-O1" :
      options.lto_level == 2 ? "--lto-O2" :
                               "--lto-O3");
    args.emplace_back("--no-lto-legacy-pass-manager");

    args.emplace_back("-prune_after_lto");
    args.emplace_back("86400"); // 1 day

    if (options.lto_cachedir && *options.lto_cachedir) {
      args.emplace_back("-cache_path_lto");
      args.emplace_back(options.lto_cachedir);

      args.emplace_back("-object_path_lto");
      args.emplace_back(mktmpstr(std::string(options.lto_cachedir) + "/obj"));
    }
  }

  // linker flavor-specific arguments
  switch (triple.getObjectFormat()) {
    case Triple::COFF:
      // flavor=lld-link
      dlog("TODO: COFF-specific args");
      return ErrNotSupported;
    case Triple::ELF:
    case Triple::Wasm:
      // flavor=ld.lld
      args.emplace_back("--no-pie");
      break;
    case Triple::MachO:
      // flavor=ld64.lld
      //args.emplace_back("-static");
      // ld64.lld: warning: Option `-static' is not yet implemented. Stay tuned...
      if (options.strip_dead) {
        // optimize
        args.emplace_back("-dead_strip"); // Remove unreferenced code and data
        // TODO: look into -mllvm "Options to pass to LLVM during LTO"
      }
      break;
    case Triple::GOFF:  // ?
    case Triple::XCOFF: // ?
    case Triple::DXContainer: // ?
    case Triple::SPIRV: // ?
    case Triple::UnknownObjectFormat:
      dlog("unexpected object format");
      return ErrNotSupported;
  }

  // OS-specific arguments
  switch (triple.getOS()) {
    case Triple::Darwin:
    case Triple::MacOSX:
      // flavor=ld64.lld
      // -platform_version <platform> <min_version> <sdk_version>
      args.emplace_back("-platform_version");
      args.emplace_back("macos");
      if (triple.getArch() == Triple::ArchType::aarch64) {
        // 11.0.0 for arm64
        args.emplace_back("11.0.0"); // see golang.org/issues/30488
        args.emplace_back("11.0.0");
        args.emplace_back( // TODO: what if this path doesn't exist?
          "-L/Library/Developer/CommandLineTools/SDKs/MacOSX.sdk/usr/lib");
      } else {
        // 10.9.0 for x86 (see golang.org/issues/30488)
        args.emplace_back("10.9.0");
        args.emplace_back("10.9.0");
      }
      args.emplace_back("-lSystem"); // macOS's "syscall API"
      //args.emplace_back("-adhoc_codesign");
      break;
    case Triple::IOS:
    case Triple::TvOS:
    case Triple::WatchOS: {
      // flavor=ld64.lld
      #if DEBUG
        CoLLVMVersionTuple mv;
        llvm_triple_min_version(options.target_triple, &mv);
        dlog("TODO min version: %d, %d, %d, %d", mv.major, mv.minor, mv.subminor, mv.build);
        // + arg "-ios_version_min" ...
      #endif
      return ErrNotSupported;
    }
    default: {
      #if DEBUG
        auto osname = (const char*)Triple::getOSTypeName(triple.getOS()).bytes_begin();
        dlog("TODO: triple.getOS()=%s", osname);
      #endif
      return ErrNotSupported;
    }
  }

  // args.emplace_back("-fno-unwind-tables");
  // args.emplace_back("-dead_strip");

  return 0;
}

// _link is a helper wrapper for calling the various object-specific linker functions
// It has been adapted from lld::safeLldMain in lld/tools/lld/lld.cpp
// Always sets errmsg
static err_t link_main(
  LinkFun linkf, llvm::ArrayRef<const char*> args, bool print_args)
{
  if (_lld_is_corrupt)
    return ErrMFault;

  #ifdef DEBUG_LLD_INVOCATION
  {
    bool first = true;
    std::string s;
    for (auto& arg : args) {
      if (first) {
        first = false;
      } else {
        s += "' '";
      }
      s += arg;
    }
    // Note: std::ostringstream with std::copy somehow adds an extra
    // empty item at the end.
    if (print_args)
      fprintf(stderr, "invoking '%s'\n", s.c_str());
  }
  #endif

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
  if (err)
    return err;

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
