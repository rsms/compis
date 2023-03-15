// SPDX-License-Identifier: Apache-2.0
#include "colib.h"
#include "target.h"
#include "strlist.h"
#include "compiler.h"
#include "path.h"
#include "llvm/llvm.h"
#include "clang/Basic/Version.inc" // CLANG_VERSION_STRING

#include <err.h>


static void diaghandler(const diag_t* d, void* nullable userdata) {
  // unused
}


int cc_main(int user_argc, char* user_argv[], bool iscxx) {
  compiler_t c;
  compiler_init(&c, memalloc_default(), &diaghandler, "main"); // FIXME pkgname

  const target_t* target = NULL;
  bool nostdlib = false;
  bool nostdinc = false;
  bool freestanding = false;
  bool nolink = false;
  bool iscompiling = false;
  bool ispastflags = false;
  bool custom_sysroot = false;
  bool no_cxx_exceptions = false;
  bool explicit_exceptions = false;
  bool explicit_cxx_exceptions = false;

  // process input command-line args
  for (int i = 1; i < user_argc; i++) {
    const char* arg = user_argv[i];
    if (*arg == '-' && !ispastflags) {
      if (streq(arg, "-nostdlib") || streq(arg, "-nolibc")) {
        nostdlib = true;
        c.opt_nostdlib = true;
      } else if (streq(arg, "-nostdinc") ||
            streq(arg, "--no-standard-includes") ||
            streq(arg, "-nostdlibinc"))
      {
        nostdinc = true;
      } else if (streq(arg, "-ffreestanding")) {
        freestanding = true;
      } else if (streq(arg, "-c") || streq(arg, "-S") || streq(arg, "-E")) {
        nolink = true;
      } else if (streq(arg, "-v") || streq(arg, "--verbose")) {
        c.opt_verbose = true;
        coverbose = true;
      } else if (streq(arg, "-O0")) {
        c.buildmode = BUILDMODE_DEBUG;
      } else if (str_startswith(arg, "-O")) {
        c.buildmode = BUILDMODE_OPT;
      } else if (streq(arg, "-fsyntax-only")) {
        nolink = true;
      } else if (streq(arg, "-fno-exceptions") || streq(arg, "-fno-cxx-exceptions")) {
        no_cxx_exceptions = true;
      } else if (streq(arg, "-fcxx-exceptions")) {
        explicit_cxx_exceptions = true;
        explicit_exceptions = true;
      } else if (streq(arg, "-fexceptions")) {
        explicit_exceptions = true;
      } else if (streq(arg, "-target")) {
        panic("TODO -target");
      } else if (streq(arg, "-sysroot") || str_startswith(arg, "--sysroot=")) {
        custom_sysroot = true;
        c.opt_nolibc = true; // don't build coroot/libc
      } else if (str_startswith(arg, "--target=")) {
        arg += strlen("--target=");
        if (!( target = target_find(arg) )) {
          log("Invalid target \"%s\"", arg);
          log("See `%s targets` for a list of supported targets", relpath(coexefile));
          return 1;
        }
      } else if (streq(arg, "--")) {
        ispastflags = true;
      }
    } else {
      const char* ext = path_ext(arg);
      if (*ext++ == '.' && *ext) {
        if (ext[1] == 0) {
          char c = *ext;
          if (c == 'c' || c == 'C' || c == 's' || c == 'S' || c == 'm' || c == 'M')
            iscompiling = true;
        } else if (strieq(ext, "cc") || strieq(ext, "cpp") || strieq(ext, "mm")) {
          iscompiling = true;
        }
      }
    }
  }

  if (!target)
    target = target_default();

  err_t err = 0;
  if (err || ( err = compiler_configure(&c, target, "build") )) {
    dlog("compiler_configure: %s", err_str(err));
    return 1;
  }

  // build system libraries, if needed
  int syslib_build_flags = 0;
  if (iscxx) syslib_build_flags |= SYSLIB_BUILD_LIBCXX;
  if (( err = build_syslibs_if_needed(&c, syslib_build_flags) )) {
    dlog("build_syslibs_if_needed: %s", err_str(err));
    return 1;
  }

  // build actual args passed to clang
  strlist_t args = strlist_make(c.ma, iscxx ? "clang++" : "clang");

  // no exception support for WASI
  if (iscompiling && iscxx && c.target.sys == SYS_wasi && !no_cxx_exceptions) {
    if (explicit_exceptions) {
      // user explicitly requested exceptions; error
      errx(1, "error: wasi target does not support exceptions [%s]",
        explicit_cxx_exceptions ? "-fcxx-exceptions" : "-fexceptions");
    }
    no_cxx_exceptions = true;
    strlist_add(&args, "-fno-exceptions");
  }

  if (freestanding) {
    // no builtins, no libc, no librt
    strlist_add(&args, "-nostdinc", "-nostdlib");
  } else {
    // add fundamental "target" compilation flags
    if (iscompiling) {
      if (!custom_sysroot) {
        strlist_add_array(&args, c.cflags_common.strings, c.cflags_common.len);
      } else {
        strlist_addf(&args, "-resource-dir=%s/clangres/", coroot);
        if (c.buildmode == BUILDMODE_OPT)
          strlist_add(&args, "-flto=thin");
      }
    }

    // add include flags for system headers and libc
    if (!nostdinc && !freestanding && !custom_sysroot) {
      if (iscxx && !c.opt_nolibcxx) {
        strlist_addf(&args, "-isystem%s/libcxx/include", coroot);
        strlist_addf(&args, "-isystem%s/libcxxabi/include", coroot);
        // strlist_addf(&args, "-isystem%s/libunwind/include", coroot);
        strlist_add(&args,
          "-D_LIBCPP_ABI_VERSION=" CO_STRX(CO_LIBCXX_ABI_VERSION),
          "-D_LIBCPP_ABI_NAMESPACE=__" CO_STRX(CO_LIBCXX_ABI_VERSION),
          "-D_LIBCPP_DISABLE_VISIBILITY_ANNOTATIONS",
          "-D_LIBCXXABI_DISABLE_VISIBILITY_ANNOTATIONS",
          "-D_LIBCPP_HAS_NO_VENDOR_AVAILABILITY_ANNOTATIONS",
          target->sys == SYS_wasi ? "-D_LIBCPP_HAS_NO_THREADS" : "",
          ( target->sys == SYS_linux || target->sys == SYS_wasi ?
            "-D_LIBCPP_HAS_MUSL_LIBC" : "")
        );
      }
      strlist_add_array(&args, c.cflags_sysinc.strings, c.cflags_sysinc.len);
    }
  }

  // linker flags
  if (!nolink) {
    char* bindir = path_dir_alloca(coexefile);
    strlist_addf(&args, "-fuse-ld=%s/%s", bindir, c.ldname);

    if (!nostdlib && !freestanding && !custom_sysroot) {
      strlist_addf(&args, "-L%s/lib", c.sysroot);
      strlist_add(&args, "-lrt");
      if (c.target.sys != SYS_none && !c.opt_nolibc)
        strlist_add(&args, "-lc");
      if (iscxx && c.target.sys != SYS_none && !c.opt_nolibcxx) {
        strlist_add(&args, "-lc++", "-lc++abi");
        if (!no_cxx_exceptions)
          strlist_add(&args, "-lunwind");
      }
    }

    switch ((enum target_sys)target->sys) {
      case SYS_macos: {
        char macosver[16];
        if (streq(target->sysver, "10")) {
          memcpy(macosver, "10.15.0", strlen("10.15.0") + 1);
        } else {
          usize i = strlen(target->sysver);
          memcpy(macosver, target->sysver, i);
          snprintf(&macosver[i], sizeof(macosver)-i, ".0.0");
        }
        strlist_addf(&args, "-Wl,-platform_version,macos,%s,%s", macosver, macosver);
        const char* arch = (target->arch == ARCH_aarch64) ? "arm64"
                                                          : arch_name(target->arch);
        strlist_addf(&args, "-Wl,-arch,%s", arch);
        break;
      }
      case SYS_linux: {
        // strlist_add(&args, "-fuse-ld=lld");
        strlist_addf(&args, "%s/lib/crt1.o", c.sysroot);
        // strlist_add(&args, "-nostdinc","-nostdlib","-ffreestanding");
        strlist_add(&args, "-nostartfiles", "-static", "-L-user-start");
        // strlist_add(&args, "-l-user-start"); // FIXME: if there are input files
        break;
      }
      case SYS_wasi:
        strlist_add(&args,
          "-Wl,--stack-first", // stack at start of linear memory to catch S.O.
          "-Wl,--export-dynamic" );
        strlist_addf(&args, "%s/lib/crt1.o", c.sysroot);
        break;
      case SYS_none:
        strlist_add(&args,
          "-ffreestanding",
          "-Wl,--no-entry");
        if (target->arch == ARCH_wasm32 || target->arch == ARCH_wasm64) {
          strlist_add(&args,
            "-Wl,--export-all", "-Wl,--no-gc-sections",
            "-Wl,--import-memory",
            "-Wl,--stack-first", // stack at start of linear memory to catch S.O.
            "-Wl,-allow-undefined"
            // -Wl,-allow-undefined-file wasm.syms // TODO: generate this?
          );
        }
        break;
    }
  }

  // append user arguments
  for (int i = 1; i < user_argc; i++) {
    const char* arg = user_argv[i];
    if (streq(arg, "-nostdlib") ||
        streq(arg, "-nolibc") ||
        streq(arg, "-nostdinc") ||
        streq(arg, "--no-standard-includes") ||
        streq(arg, "-nostdlibinc") ||
        str_startswith(arg, "--target=") )
    {
      // skip single-arg flag
    } else if (streq(arg, "-target")) {
      // skip double-arg flag
      i++;
    } else {
      strlist_add(&args, arg);
    }
  }

  if (target->sys == SYS_linux && !nolink) {
    // strlist_add(&args, "-l-user-end"); // FIXME: if there are input files
    strlist_add(&args, "-L-user-end");
  }

  // build argv array
  char* const* argv = strlist_array(&args);
  if (!args.ok)
    return dlog("strlist_array failed"), 1;

  #if DEBUG
  dlog("invoking %s", argv[0]);
  for (u32 i = 1; i < args.len; i++)
    fprintf(stderr, "  %s\n", argv[i]);
  #endif

  // invoke clang
  return clang_main((int)args.len, argv);
}
