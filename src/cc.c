// SPDX-License-Identifier: Apache-2.0
#include "colib.h"
#include "target.h"
#include "strlist.h"
#include "compiler.h"
#include "path.h"
#include "llvm/llvm.h"
#include "clang/Basic/Version.inc" // CLANG_VERSION_STRING

#include <unistd.h> // symlink


static void diaghandler(const diag_t* d, void* nullable userdata) {
  // unused
}


static bool streq(const char* a, const char* b) {
  return strcmp(a, b) == 0;
}


int cc_main(int user_argc, char* user_argv[]) {
  compiler_t c;
  compiler_init(&c, memalloc_default(), &diaghandler, "main"); // FIXME pkgname

  const target_t* target = target_default();
  bool nostdlib = false;
  bool nostdinc = false;
  bool freestanding = false;
  bool nolink = false;

  // process input command-line args
  for (int i = 1; i < user_argc; i++) {
    const char* arg = user_argv[i];
    if (streq(arg, "-nostdlib") || streq(arg, "-nolibc")) {
      nostdlib = true;
    } if (streq(arg, "-nostdinc") ||
          streq(arg, "--no-standard-includes") ||
          streq(arg, "-nostdlibinc"))
    {
      nostdinc = true;
    } if (streq(arg, "-ffreestanding")) {
      freestanding = true;
    } if (streq(arg, "-c") || streq(arg, "-S") || streq(arg, "-E")) {
      nolink = true;
    } else if (streq(arg, "-target")) {
      panic("TODO -target");
    } else if (str_startswith(arg, "--target=")) {
      arg += strlen("--target=");
      if (!( target = target_find(arg) )) {
        log("Invalid target \"%s\"", arg);
        log("See `%s targets` for a list of supported targets", relpath(coexefile));
        return 1;
      }
    }
  }

  err_t err = 0;
  if (err || ( err = compiler_configure(&c, target, "build") )) {
    dlog("compiler_configure: %s", err_str(err));
    return 1;
  }

  if (( err = build_syslibs_if_needed(&c) )) {
    dlog("build_syslibs_if_needed: %s", err_str(err));
    return 1;
  }

  // build actual args passed to clang
  strlist_t args = strlist_make(c.ma, "clang");

  if (freestanding) {
    // no builtins, no libc, no librt
    strlist_add(&args, "-nostdinc", "-nostdlib");
  } else {
    // add fundamental "target" compilation flags
    strlist_add_array(&args, c.cflags_common.strings, c.cflags_common.len);

    // add include flags for system headers and libc
    if (!nostdinc && !freestanding)
      strlist_add_array(&args, c.cflags_sysinc.strings, c.cflags_sysinc.len);

    // add linker flags
    if (nolink || (!nostdlib && !freestanding)) {
      strlist_addf(&args, "-L%s/lib", c.sysroot);
      strlist_add(&args, "-lc", "-lrt");
    }
  }

  // linker flags
  if (!nolink) {
    // create lld symlink
    // TODO: move to build_syslibs
    const char* ldexefile;
    switch ((enum target_sys)target->sys) {
      case SYS_linux: ldexefile = "ld.lld"; break;
      case SYS_macos: ldexefile = "ld64.lld"; break;
      // case SYS_wasm: case SYS_wasi: ldexefile = "wasm-ld"; break;
      // case SYS_win32: ldexefile = "lld-link"; break;
      case SYS_COUNT: case SYS_none: safefail("bad sys"); break;
    }
    ldexefile = path_join_alloca(c.sysroot, ldexefile);
    symlink(coexefile, ldexefile);
    strlist_addf(&args, "-fuse-ld=%s", ldexefile);

    if (target->sys == SYS_macos && !nolink) {
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
    } else if (target->sys == SYS_linux && !nolink) {
      // strlist_add(&args, "-fuse-ld=lld");
      strlist_addf(&args, "%s/lib/crt1.o", c.sysroot);
      // strlist_add(&args, "-nostdinc","-nostdlib","-ffreestanding");
      strlist_add(&args, "-nostartfiles", "-static", "-L-user-start");
      // strlist_add(&args, "-l-user-start"); // FIXME: if there are input files
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
        streq(arg, "-lc") ||
        streq(arg, "-lrt") ||
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
