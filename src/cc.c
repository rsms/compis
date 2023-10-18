// SPDX-License-Identifier: Apache-2.0
#include "colib.h"
#include "target.h"
#include "strlist.h"
#include "compiler.h"
#include "path.h"
#include "llvm/llvm.h"
#include "clang/Basic/Version.inc" // CLANG_VERSION_STRING

#include <err.h>
#include <errno.h>
#include <stdlib.h>
#include <unistd.h>


static void diaghandler(const diag_t* d, void* nullable userdata) {
  // unused
}


#define die(fmt, args...) { \
  elog("%s: " fmt, coprogname, ##args); \
  exit(1); \
}


static err_t symlink_ld(compiler_t* c) {
  if (*c->ldname == 0)
    return 0;

  const char* target = path_base_cstr(coexefile); // e.g. "compis"

  // linkfile = path_dir(coexefile) "/" c->ldname
  char linkfile[PATH_MAX];
  usize n = path_dir_buf(linkfile, sizeof(linkfile), coexefile);
  usize ldnamelen = strlen(c->ldname);
  if (n + 1 + ldnamelen >= sizeof(linkfile))
    return ErrOverflow;
  linkfile[n++] = PATH_SEP;
  memcpy(&linkfile[n], c->ldname, ldnamelen + 1);

  if (symlink(target, linkfile) != 0) {
    if (errno != EEXIST)
      return err_errno();
  } else {
    vlog("symlink %s -> %s", relpath(linkfile), target);
  }

  return 0;
}


static char* add_trailing_slash(memalloc_t ma, char* s) {
  usize len = strlen(s);
  if (len > 0 && s[len - 1] != PATH_SEP) {
    s = mem_strdup(ma, (slice_t){{s},len}, 1);
    s[len] = PATH_SEP;
    s[len + 1] = 0;
  }
  return s;
}


int cc_main(int user_argc, char* user_argv[], bool iscxx) {
  compiler_t c;
  compiler_init(&c, memalloc_default(), &diaghandler);

  compiler_config_t config = {0};
  config.buildmode = BUILDMODE_OPT; // default to optimized build
  config.nolto = true; // disable LTO for implicit -O0 (enabled for -O1+)

  const target_t* target = NULL;

  bool link = true;
  bool link_libc = true;
  bool link_libcxx = iscxx;
  bool link_librt = true;
  bool startfiles = true;
  bool nostdinc = false;
  bool custom_ld = false;
  bool freestanding = false;
  bool cxx_exceptions = true;
  const char* nullable enable_modules = NULL; // if set, it's the first argv seen
  bool explicit_exceptions = false;
  bool explicit_cxx_exceptions = false;
  const char* nullable custom_sysroot = NULL;

  bool iscompiling = false;
  bool ispastflags = false;
  bool print_only = false;
  bool has_link_flags = false;

  // process input command-line args
  // https://clang.llvm.org/docs/ClangCommandLineReference.html
  // https://gcc.gnu.org/onlinedocs/gcc/Invoking-GCC.html
  // https://gcc.gnu.org/onlinedocs/gcc/Directory-Options.html
  for (int i = 1; i < user_argc; i++) {
    char* arg = user_argv[i];
    if (*arg == '-' && !ispastflags) {

      // general options
      if (streq(arg, "-v") || streq(arg, "--verbose")) {
        config.verbose = true;
        coverbose = MAX(coverbose, 1);
      } else if (streq(arg, "-vv")) {
        // note: -vv is compis-specific and is converted to "-v"
        arg[2] = 0; // "-vv" -> "-v"
        config.verbose = true;
        coverbose = MAX(coverbose, 2);
      } else if (streq(arg, "-###")) {
        config.verbose = true;
        coverbose = MAX(coverbose, 2);
        print_only = true;
      } else if (streq(arg, "--help") || streq(arg, "-help")) {
        print_only = true;
      }

      // linker flags
      else if (streq(arg, "-nostartfiles")) {
        // Do not use the standard system startup files when linking.
        // The standard system libraries are used normally, unless -nostdlib, -nolibc,
        // or -nodefaultlibs is used.
        startfiles = false;
      } else if (streq(arg, "-nodefaultlibs")) {
        // Do not use the standard system libraries when linking.
        // Only the libraries you specify are passed to the linker, and options
        // specifying linkage of the system libraries are ignored.
        // The standard startup files are used normally, unless -nostartfiles is used.
        link_librt = false;
        link_libc = false;
        link_libcxx = false;
      } else if (streq(arg, "-nolibc")) {
        // Do not use the C library or system libraries tightly coupled with it when
        // linking. Still link with the startup files, librt and libstdc++ unless
        // options preventing their inclusion are used as well.
        link_libc = false;
      } else if (streq(arg, "-nostdlib") || streq(arg, "--no-standard-libraries")) {
        // Do not use the standard system startup files or libraries when linking.
        // No startup files and only the libraries you specify are passed to the linker,
        // and options specifying linkage of the system libraries are ignored.
        link_librt = false;
        link_libc = false;
        link_libcxx = false;
        startfiles = false;
      } else if (streq(arg, "-nostdlib++")) {
        // Do not implicitly link with standard C++ libraries
        link_libcxx = false;
      } else if (streq(arg, "-fno-lto")) {
        config.nolto = true;
      } else if (string_startswith(arg, "-fuse-ld=")) {
        custom_ld = true;
        // must disable LTO, or else clang complains:
        //   "error: 'x86_64-unknown': unable to pass LLVM bit-code files to linker"
        config.nolto = true;
      }

      // compilation flags
      else if (streq(arg, "-nostdinc") ||
               streq(arg, "--no-standard-includes") ||
               streq(arg, "-nostdlibinc"))
      {
        // Do not search the standard system directories for header files.
        // Only the directories explicitly specified with -I, -iquote, -isystem,
        // and/or -idirafter options (and the directory of the current file,
        // if appropriate) are searched.
        nostdinc = true;
      } else if (streq(arg, "-ffreestanding")) {
        // Assert that compilation targets a freestanding environment.
        // This implies -fno-builtin. A freestanding environment is one in which the
        // standard library may not exist, and program startup may not necessarily be
        // at main.
        freestanding = true;
      } else if (streq(arg, "-c") || streq(arg, "-S") || streq(arg, "-E")) {
        // -c "only compile"
        // -S "only assemble"
        // -E "only preprocess"
        link = false;
        iscompiling = true;
      } else if (streq(arg, "-x")) {
        iscompiling = true;
      } else if (streq(arg, "-L") || streq(arg, "-l")) {
        has_link_flags = true;
      } else if (streq(arg, "-o")) {
        // infer "no linking" based on output filename
        if (i+1 < user_argc) {
          arg = user_argv[i+1];
          const char* ext = path_ext_cstr(arg);
          if (*ext++ && ( // guard ""
            strieq(ext, "o") ||
            strieq(ext, "pch") ||
            strieq(ext, "h") ||
            strieq(ext, "hh") ||
            strieq(ext, "hpp") ))
          {
            link = false;
          }
        }
      } else if (streq(arg, "-O0")) {
        config.nolto = true;
      } else if (string_startswith(arg, "-O")) {
        config.nolto = false;
      } else if (streq(arg, "--co-debug")) {
        config.buildmode = BUILDMODE_DEBUG;
      } else if (streq(arg, "-fsyntax-only")) {
        link = false;
      } else if (streq(arg, "-fno-exceptions") || streq(arg, "-fno-cxx-exceptions")) {
        cxx_exceptions = false;
      } else if (streq(arg, "-fcxx-exceptions")) {
        explicit_cxx_exceptions = true;
        explicit_exceptions = true;
      } else if (streq(arg, "-fexceptions")) {
        explicit_exceptions = true;
      } else if (string_startswith(arg, "-mmacosx-version-min=")) {
        // TODO: parse and check that: value <= target.sysver && value >= minver(target)
        config.sysver = arg + strlen("-mmacosx-version-min=");
      }

      // flags that affect both compilation and linking
      else if (string_startswith(arg, "--target=") || streq(arg, "-target")) {
        if (arg[1] == '-') { // --target=...
          arg += strlen("--target=");
        } else {
          if (i + 1 == user_argc)
            break;
          arg = user_argv[i+1];
        }
        if (!( target = target_find(arg) )) {
          log("Invalid target \"%s\"", arg);
          log("See `%s targets` for a list of supported targets", relpath(coexefile));
          return 1;
        }
      } else if (streq(arg, "--sysroot")) {
        // a bug (or shortcoming?) in clang causes clang's driver to populate system
        // search directories assuming sysroot ends in "/", which it often does not.
        // (For example cmake will strip trailing slashes in CMAKE_OSX_SYSROOT.)
        if (i+1 < user_argc)
          custom_sysroot = user_argv[i+1] = add_trailing_slash(c.ma, user_argv[i+1]);
      } else if (streq(arg, "-isysroot")) {
        // This option is like the --sysroot option, but applies only to header files.
        // On Darwin targets it applies to both header files and libraries.
        if (i+1 < user_argc)
          custom_sysroot = user_argv[i+1] = add_trailing_slash(c.ma, user_argv[i+1]);
      } else if (string_startswith(arg, "--sysroot=")) {
        user_argv[i] = add_trailing_slash(c.ma, user_argv[i]);
        custom_sysroot = user_argv[i] + strlen("--sysroot=");
      } else if (streq(arg, "-fno-lto")) {
        config.nolto = true;
      } else if (string_startswith(arg, "-flto")) {
        config.nolto = false;
        if (config.buildmode == BUILDMODE_DEBUG)
          die("error: %s cannot be used together with --co-debug", arg);
      } else if (streq(arg, "-fmodules") || streq(arg, "-fcxx-modules")) {
        if (enable_modules == NULL)
          enable_modules = arg;
      } else if (streq(arg, "--")) {
        ispastflags = true;
      }
    }

    // non-option args (does not start with "-" OR we have seen "--")
    else {
      const char* ext = path_ext_cstr(arg);
      if (*ext && *ext == '.' && ext[1]) { // unless "" or "."
        ext++; // skip past "."
        if (ext[1] == 0) {
          char c = *ext;
          if (c == 'c' || c == 'C' || c == 's' || c == 'S' || c == 'm' || c == 'M') {
            iscompiling = true;
          } else if (c == 'h' || c == 'H') {
            iscompiling = true;
            link = false;
          }
        } else if (
          strieq(ext, "cc") || strieq(ext, "cpp") || strieq(ext, "mm") ||
          strieq(ext, "pch") || strieq(ext, "hh") || strieq(ext, "hpp") )
        {
          iscompiling = true;
        }
      }
    }
  } // end of argv loop

  // if any link flags are given, then infer invocation as "linking"
  if (has_link_flags)
    link = true;

  // check if modules are enabled
  if (enable_modules && !custom_sysroot) {
    die("error: %s is not yet supported"
        "(unless using a custom --sysroot with module support)",
        enable_modules);
  }

  // if no target is set, use the host
  if (!target)
    target = target_default();

  // update link_LIB vars depending on target
  link_librt  = link_librt  && target_has_syslib(target, SYSLIB_RT);
  link_libc   = link_libc   && !freestanding && target_has_syslib(target, SYSLIB_C);
  link_libcxx = link_libcxx && !freestanding && target_has_syslib(target, SYSLIB_CXX);
  // FIXME: link_libcxx is also used to enable/disable C++ header inclusion

  // configure compiler
  config.target = target;
  config.buildroot = "build-THIS-IS-A-BUG-IN-COMPIS"; // should never be used
  config.nolibc = !link_libc;
  config.nolibcxx = !link_libcxx;
  config.sysroot = custom_sysroot;

  err_t err = 0;
  if (err || ( err = compiler_configure(&c, &config) ))
    die("compiler_configure: %s", err_str(err));

  // print config in -v mode
  char targetstr[TARGET_FMT_BUFCAP];
  if (coverbose) {
    target_fmt(target, targetstr, sizeof(targetstr));
    printf("compis invoked as: %s\n", coprogname);
    printf("compis executable: %s\n", coexefile);
    printf("target: %s\n", targetstr);
    printf("COMAXPROC=%u\n", comaxproc);
    printf("COROOT=%s\n", coroot);
    printf("COCACHE=%s\n", cocachedir);
    printf("sysroot=%s\n", c.sysroot);
  }

  // build sysroot
  if (!custom_sysroot && !print_only) {
    int sysroot_build_flags = 0;
    if (link_libcxx)
      sysroot_build_flags |= SYSROOT_ENABLE_CXX;
    if (( err = build_sysroot_if_needed(&c, sysroot_build_flags) ))
      die("failed to configure sysroot: %s", err_str(err));
  }

  // build actual args passed to clang
  strlist_t args = strlist_make(c.ma, iscxx ? "clang++" : "clang");

  // no exception support for WASI
  if (iscompiling && iscxx && c.target.sys == SYS_wasi && cxx_exceptions) {
    if (explicit_exceptions) {
      // user explicitly requested exceptions; error
      errx(1, "error: wasi target does not support exceptions [%s]",
        explicit_cxx_exceptions ? "-fcxx-exceptions" : "-fexceptions");
    }
    cxx_exceptions = false;
    strlist_add(&args, "-fno-exceptions");
  }

  if (freestanding) {
    // no builtins, no libc, no librt
    strlist_add(&args, "-nostdinc", "-nostdlib");
    // add minimal args needed (from compiler.c's configure_cflags)
    strlist_addf(&args, "-B%s", coroot);
    strlist_addf(&args, "--target=%s", c.target.triple);
    strlist_addf(&args, "-resource-dir=%s/clangres/", coroot);
  } else {
    // add fundamental "target" compilation flags
    if (iscompiling && !custom_sysroot) {
      strlist_add_array(&args, c.cflags_common.strings, c.cflags_common.len);
    } else {
      strlist_add_array(&args, c.flags_common.strings, c.flags_common.len);
    }

    // add include flags for system headers and libc
    if (!nostdinc) {
      if (link_libcxx) {
        // We need to specify C++ include directories here so that they are searched
        // before clang resource dir. If we don't do this, the wrong cstddef header
        // will be used and we'll see errors like this:
        //   "error: no member named 'nullptr_t' in the global namespace"
        strlist_addf(&args, "-isystem%s/libcxx/include", coroot);
        strlist_addf(&args, "-isystem%s/libcxxabi/include", coroot);
        strlist_addf(&args, "-isystem%s/libunwind/include", coroot);
      }
      if (!custom_sysroot) {
        strlist_add_array(&args, c.cflags_sysinc.strings, c.cflags_sysinc.len);
        strlist_addf(&args, "-isystem%s/clangres/include", coroot);
      }
    }
  }

  // linker flags
  if (link) {

    // configure linker
    if (!custom_ld) {
      if (*c.ldname == 0) {
        target_fmt(target, targetstr, sizeof(targetstr));
        die("no linker available for target %s", targetstr);
      }
      // create symlink for linker invocation, required for clang driver to work
      if (!print_only && ( err = symlink_ld(&c) ))
        die("failed to create linker symlink: %s", err_str(err));
      char* bindir = path_dir_alloca(coexefile);
      // disable warning
      //   compis: warning: '-fuse-ld=' taking a path is deprecated;
      //   use '--ld-path=' instead [-Wfuse-ld-path]
      strlist_add(&args, "-Wno-fuse-ld-path");
      strlist_addf(&args, "-fuse-ld=%s/%s", bindir, c.ldname); // TODO: just ldname?
    }

    strlist_add(&args, "-nodefaultlibs");

    if (link_librt || link_libc || link_libcxx) {
      if (custom_sysroot) {
        str_t libdir = path_join(c.sysroot, "lib");
        if (fs_isdir(libdir.p)) {
          strlist_addf(&args, "-L%s", libdir.p);
        } else {
          str_free(libdir);
          libdir = path_join(c.sysroot, "usr", "lib");
          if (fs_isdir(libdir.p)) {
            strlist_addf(&args, "-L%s", libdir.p);
          } else {
            libdir.len = 0;
          }
        }
        if (link_librt && libdir.len > 0) {
          // TODO: build librt separately from other syslibs
          // and use it even for custom sysroots as it's libc-independent.
          // Also make sure that config.sysver is honred.
          str_t libfile = path_join(libdir.p, "librt.a");
          if (fs_isfile(libfile.p))
            strlist_add(&args, "-lrt");
          str_free(libfile);
        }
        str_free(libdir);
      } else {
        strlist_addf(&args, "-L%s/lib", c.sysroot);
        if (link_librt)
          strlist_add(&args, "-lrt");
      }
      if (link_libc) {
        strlist_add(&args, "-lc");
        if (link_libcxx) {
          strlist_add(&args, "-lc++", "-lc++abi");
          if (cxx_exceptions)
            strlist_add(&args, "-lunwind");
        }
      }
    }

    switch ((enum target_sys)target->sys) {
      case SYS_macos: {
        if (!custom_sysroot) {
          char macosver[16];
          target_llvm_version(target, macosver);
          const char* minver = macosver;
          if (config.sysver && *config.sysver)
            minver = config.sysver;
          strlist_addf(&args, "-Wl,-platform_version,macos,%s,%s", minver, macosver);
          const char* arch = (target->arch == ARCH_aarch64) ? "arm64"
                                                            : arch_name(target->arch);
          strlist_addf(&args, "-Wl,-arch,%s", arch);
        }
        break;
      }
      case SYS_linux: {
        if (startfiles)
          strlist_addf(&args, "%s/lib/crt1.o", c.sysroot);
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
    // skip arguments which are managed by compiler.c, or handled specially
    const char* arg = user_argv[i];
    if (streq(arg, "-nostdlib") ||
        streq(arg, "-nolibc") ||
        streq(arg, "-nostdinc") ||
        streq(arg, "--no-standard-includes") ||
        streq(arg, "-nostdlibinc") ||
        streq(arg, "--co-debug") ||
        string_startswith(arg, "-mmacosx-version-min=") ||
        string_startswith(arg, "--target=") )
    {
      // skip single-arg flag
    } else if (streq(arg, "-target")) {
      // skip double-arg flag
      i++;
    } else {
      strlist_add(&args, arg);
    }
  }

  if (target->sys == SYS_linux && link) {
    // strlist_add(&args, "-l-user-end"); // FIXME: if there are input files
    strlist_add(&args, "-L-user-end");
  }

  // build argv array
  char* const* argv = strlist_array(&args);
  if (!args.ok)
    return dlog("strlist_array failed"), 1;

  // print effective clang invocation in -vv and -### mode
  if (coverbose > 1) {
    printf("compis cc exec: %s%s", argv[0], args.len > 0 ? " \\" : "");
    for (u32 i = 1; i < args.len; i++)
      printf("  %s%s\n", argv[i], i+1 < args.len ? " \\" : "");
  }

  // invoke clang
  return clang_main((int)args.len, argv);
}
