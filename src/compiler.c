// SPDX-License-Identifier: Apache-2.0
#include "colib.h"
#include "compiler.h"
#include "path.h"
#include "subproc.h"
#include "llvm/llvm.h"
#include "clang/Basic/Version.inc" // CLANG_VERSION_STRING

#include <err.h>
#include <errno.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

#include <string.h> // XXX


void compiler_init(compiler_t* c, memalloc_t ma, diaghandler_t dh) {
  memset(c, 0, sizeof(*c));
  c->ma = ma;
  c->diaghandler = dh;
  buf_init(&c->diagbuf, c->ma);
  safecheckxf(rwmutex_init(&c->diag_mu) == 0, "rwmutex_init");
  safecheckxf(locmap_init(&c->locmap) == 0, "locmap_init");
  safecheckxf(rwmutex_init(&c->pkgindex_mu) == 0, "rwmutex_init");
  safecheckxf(map_init(&c->pkgindex, c->ma, 32), "map_init");
}


void compiler_dispose(compiler_t* c) {
  buf_dispose(&c->diagbuf);
  locmap_dispose(&c->locmap, c->ma);
  strlist_dispose(&c->cflags_all);
  mem_freecstr(c->ma, c->buildroot);
  mem_freecstr(c->ma, c->builddir);
  mem_freecstr(c->ma, c->sysroot);
  rwmutex_dispose(&c->diag_mu);
  locmap_dispose(&c->locmap, c->ma);

  for (const mapent_t* e = map_it(&c->pkgindex); map_itnext(&c->pkgindex, &e); )
    pkg_dispose(e->value, c->ma);
  map_dispose(&c->pkgindex, c->ma);
  rwmutex_dispose(&c->pkgindex_mu);

  if (c->u8stype.mangledname)
    mem_freex(c->ma, MEM(c->u8stype.mangledname, strlen(c->u8stype.mangledname) + 1));
  if (c->strtype.mangledname)
    mem_freex(c->ma, MEM(c->strtype.mangledname, strlen(c->strtype.mangledname) + 1));
}


static err_t set_secondary_pointer_types(compiler_t* c) {
  // "&[u8]" -- slice of u8 array
  memset(&c->u8stype, 0, sizeof(c->u8stype));
  c->u8stype.kind = TYPE_SLICE;
  c->u8stype.is_builtin = true;
  c->u8stype.flags = NF_CHECKED | NF_VIS_PUB;
  c->u8stype.size = c->target.ptrsize;
  c->u8stype.align = c->target.ptrsize;
  c->u8stype.elem = type_u8;

  pkg_t pkg = {0};
  c->diagbuf.len = 0;
  safecheckx(compiler_mangle(c, &pkg, &c->diagbuf, (node_t*)&c->u8stype));
  c->u8stype.mangledname = mem_strdup(c->ma, buf_slice(c->diagbuf), 0);
  safecheck(c->u8stype.mangledname);

  // "type str &[u8]"
  memset(&c->strtype, 0, sizeof(c->strtype));
  c->strtype.kind = TYPE_ALIAS;
  c->strtype.is_builtin = true;
  c->strtype.flags = NF_CHECKED | NF_VIS_PUB;
  c->strtype.size = c->target.ptrsize;
  c->strtype.align = c->target.ptrsize;
  c->strtype.name = sym_str;
  c->strtype.elem = (type_t*)&c->u8stype;
  c->strtype.mangledname = mem_strdup(c->ma, slice_cstr(CO_MANGLEDNAME_STR), 0);
  safecheck(c->strtype.mangledname);

  return 0;
}


static err_t configure_target(compiler_t* c, const compiler_config_t* config) {
  c->target = *config->target;
  switch (c->target.ptrsize) {
    case 1:
      c->addrtype = type_u8;
      c->uinttype = type_u8;
      c->inttype  = type_i8;
      break;
    case 2:
      c->addrtype = type_u16;
      c->uinttype = type_u16;
      c->inttype  = type_i16;
      break;
    case 4:
      c->addrtype = type_u32;
      c->uinttype = type_u32;
      c->inttype  = type_i32;
      break;
    default:
      assert(c->target.ptrsize <= 8);
      c->addrtype = type_u64;
      c->uinttype = type_u64;
      c->inttype  = type_i64;
  }

  err_t err = set_secondary_pointer_types(c);
  if (err)
    return err;

  if (c->target.sys == SYS_none) {
    c->opt_nolibc = true;
    c->opt_nolibcxx = true;
  }

  c->ldname = target_linker_name(&c->target);

  if (target_is_riscv(&c->target) && isatty(STDOUT_FILENO)) {
    elog("%s: warning: RISC-V support is experimental", coprogname);
  } else if (target_is_arm(&c->target) && isatty(STDOUT_FILENO)) {
    elog("%s: warning: ARM support is experimental", coprogname);
  }

  // enable LTO for optimized builds.
  // RISC-V is disabled because lld fails with float ABI errors.
  // ARM is disabled becaues lld crashes when trying to LTO link.
  if (c->buildmode == BUILDMODE_OPT &&
      !config->nolto &&
      !target_is_riscv(&c->target) &&
      !target_is_arm(&c->target))
  {
    c->lto = 2;
  } else {
    c->lto = 0;
  }

  return 0;
}


static const char* buildmode_name(buildmode_t m) {
  switch ((enum buildmode)m) {
    case BUILDMODE_DEBUG: return "debug";
    case BUILDMODE_OPT:   return "opt";
  }
  return "unknown";
}


static err_t configure_sysroot(compiler_t* c, const compiler_config_t* config) {
  // custom sysroot
  if (config->sysroot && *config->sysroot) {
    char* sysroot = mem_strdup(c->ma, slice_cstr(config->sysroot), 0);
    if (!sysroot)
      return ErrNoMem;
    c->sysroot = sysroot;
    return 0;
  }

  // automatic sysroot
  // sysroot = cocachedir "/" target "-lto"? "-debug"?
  str_t sysroot = str_make(cocachedir);
  bool ok = str_push(&sysroot, PATH_SEP);
  if (( ok &= str_ensure_avail(&sysroot, TARGET_FMT_BUFCAP) ))
    sysroot.len += target_fmt(&c->target, str_end(sysroot), TARGET_FMT_BUFCAP);
  if (c->lto > 0)
    ok &= str_append(&sysroot, "-lto");
  if (c->buildmode == BUILDMODE_DEBUG)
    ok &= str_append(&sysroot, "-debug");
  if (!ok)
    return ErrNoMem;

  mem_freecstr(c->ma, c->sysroot);
  c->sysroot = mem_strdup(c->ma, str_slice(sysroot), 0);
  str_free(sysroot);
  return c->sysroot ? 0 : ErrNoMem;
}


static err_t configure_cflags(compiler_t* c, const compiler_config_t* config) {
  strlist_dispose(&c->cflags_all);
  c->cflags_all = strlist_make(c->ma);
  strlist_t* cflags_all = &c->cflags_all;

  // flags used for all C and assembly compilation
  strlist_addf(cflags_all, "-B%s", coroot);
  strlist_addf(cflags_all, "--target=%s", c->target.triple);
  strlist_addf(cflags_all, "--sysroot=%s/", c->sysroot);
  strlist_addf(cflags_all, "-resource-dir=%s/clangres/", coroot);
  strlist_add(cflags_all, "-nostdlib");

  if (c->target.sys == SYS_macos) {
    // set -mmacosx-version-min=version rather than embedding the target version
    // in target.triple. This allows separating the sysroot version from the
    // minimum supported OS version.
    // For example, libdawn must be build with the macos 11 SDK when targeting macos 10.
    const char* sysver = config->sysver;
    char sysverbuf[16];
    if (!sysver || *sysver == 0) {
      target_llvm_version(&c->target, sysverbuf);
      sysver = sysverbuf;
    }
    strlist_addf(cflags_all, "-mmacosx-version-min=%s", sysver);
    // disable "nullability completeness" warnings by default
    strlist_add(cflags_all, "-Wno-nullability-completeness");
  }

  if (c->lto)
    strlist_add(cflags_all, "-flto=thin");

  // RISC-V has a bunch of optional features
  // https://gcc.gnu.org/onlinedocs/gcc/RISC-V-Options.html
  if (c->target.arch == ARCH_riscv64) {
    // e       RV32E (16 gp regs instead of 32)
    // i       RV64
    // a       Atomic Instructions
    // c       Compressed Instructions
    // f       Single-Precision Floating-Point
    // d       Double-Precision Floating-Point (requires f)
    // m       Integer Multiplication and Division
    // v       Vector Extension for Application Processors
    //         (requires d, zve64d, zvl128b)
    // relax   Enable Linker relaxation
    // zvl32b  Minimum Vector Length 32
    // zvl64b  Minimum Vector Length 64 (requires zvl32b)
    // zvl128b Minimum Vector Length 128 (requires zvl64b)
    // zve32x  Vector Extensions for Embedded Processors with maximal 32 EEW
    //         (requires zvl32b)
    // zve32f  Vector Extensions for Embedded Processors with maximal 32 EEW
    //         and F extension. (requires zve32x)
    // zve64x  Vector Extensions for Embedded Processors with maximal 64 EEW
    //         (requires zve32x, zvl64b)
    // zve64f  Vector Extensions for Embedded Processors with maximal 64 EEW
    //         and F extension. (requires zve32f, zve64x)
    // zve64d  Vector Extensions for Embedded Processors with maximal 64 EEW,
    //         F and D extension. (requires zve64f)
    // ... AND A LOT MORE ...
    strlist_add(cflags_all,
      "-march=rv64iafd",
      "-mabi=lp64d", // ilp32d for riscv32
      "-mno-relax",
      "-mno-save-restore");
  } else if (target_is_arm(&c->target)) {
    strlist_add(cflags_all,
      "-march=armv6",
      "-mfloat-abi=hard",
      "-mfpu=vfp");
  }

  // end of common flags
  u32 flags_common_end = c->cflags_all.len;

  // ————— start of common cflags —————
  if (c->buildmode == BUILDMODE_OPT)
    strlist_add(cflags_all, "-D_FORTIFY_SOURCE=2");
  if (!target_has_syslib(&c->target, SYSLIB_C)) {
    // invariant: c->opt_nostdlib=true (when c->target.sys == SYS_none)
    strlist_add(cflags_all,
      "-nostdinc",
      "-ffreestanding");
    // note: must add <resdir>/include explicitly when -nostdinc is set
    strlist_addf(cflags_all, "-isystem%s/clangres/include", coroot);
  }

  // end of common cflags
  u32 cflags_common_end = c->cflags_all.len;

  // ————— start of cflags_sysinc —————

  // DISABLED, for now.
  // Including this for all source files causes problems with feature detection
  // code like that found in zlib's zutil.h.
  // Maybe it can be conditionally included for objc compilation?
  // Maybe we should include ConditionalMacros.h instead(it in turn includes
  // TargetConditionals.h)
  //
  // // macos assumes constants in TargetConditionals.h are predefined.
  // if (c->target.sys == SYS_macos && !c->opt_nolibc)
  //   strlist_add(cflags_all, "-include", "TargetConditionals.h");

  // end of cflags_sysinc
  u32 cflags_sysinc_end = c->cflags_all.len;

  // ————— start of cflags_c (for compis .c ) —————
  strlist_add(cflags_all,
    "-std=c17",
    "-g",
    "-feliminate-unused-debug-types");
  switch ((enum buildmode)c->buildmode) {
    case BUILDMODE_DEBUG:
      strlist_add(cflags_all, "-O0");
      break;
    case BUILDMODE_OPT:
      strlist_add(cflags_all, "-O2", "-fomit-frame-pointer");
      break;
  }
  #if !defined(CO_DISTRIBUTION)
    // coprelude.h is included in c->builddir but during development it
    // is useful to be able to modify coprelude.h directly.
    strlist_addf(cflags_all, "-isystem%s/compis", coroot);
  #endif
  strlist_addf(cflags_all, "-isystem%s", c->builddir); // i.e. pkg/foo/pub.h

  u32 cflags_c_end = c->cflags_all.len;
  // end of cflags

  // ————— start of cflags_co (for compis .co.c) —————

  // include coprelude.h in all .co.c files
  #if defined(CO_DISTRIBUTION)
    strlist_addf(cflags_all, "-include%s/include/coprelude.h", c->sysroot);
  #else
    strlist_addf(cflags_all, "-include%s/compis/coprelude.h", coroot);
  #endif
  u32 cflags_co_end = c->cflags_all.len;
  // end of cflags_compis

  // make slices
  const char*const* argv = (const char*const*)strlist_array(cflags_all);
  if (!c->cflags_all.ok)
    return ErrNoMem;
  c->flags_common = (slice_t){ .strings = argv, .len = (usize)flags_common_end };
  c->cflags_common = (slice_t){ .strings = argv, .len = (usize)cflags_common_end };
  c->cflags_sysinc = (slice_t){
    .strings = &argv[cflags_common_end],
    .len = (usize)(cflags_sysinc_end - cflags_common_end),
  };
  c->cflags_c = (slice_t){ .strings = argv, .len = (usize)cflags_c_end };
  c->cflags_co = (slice_t){ .strings = argv, .len = (usize)cflags_co_end };

  return 0;
}


static void configure_builtin_functions(compiler_t* c) {
  static local_t this_param = {}; // this
  static local_t this_param_mut = {}; // mut this
  static local_t uint_param = {}; // _ uint
  static node_t* params1[1] = {}; // (this)
  static node_t* params2[2] = {}; // (mut this, _ uint)

  if (this_param.kind == 0) {
    this_param = (local_t){
      .kind = EXPR_PARAM,
      .is_builtin = true,
      .flags = NF_CHECKED,
      .name = sym_this,
      .isthis = true,
      .type = type_unknown,
    };
    this_param_mut = (local_t){
      .kind = EXPR_PARAM,
      .is_builtin = true,
      .flags = NF_CHECKED,
      .name = sym_this,
      .isthis = true,
      .ismut = true,
      .type = type_unknown,
    };
    uint_param = (local_t){
      .kind = EXPR_PARAM,
      .is_builtin = true,
      .flags = NF_CHECKED,
      .name = sym__,
      .type = type_uint,
    };
    params1[0] = (node_t*)&this_param;
    params2[0] = (node_t*)&this_param_mut;
    params2[1] = (node_t*)&uint_param;
  }

  // [type] fun(this)uint
  c->funtype1 = (funtype_t){
    .kind = TYPE_FUN,
    .is_builtin = true,
    .flags = NF_VIS_PUB | NF_CHECKED,
    .size = c->target.ptrsize,
    .align = c->target.ptrsize,
    .mangledname = (char*)(CO_ABI_GLOBAL_PREFIX "builtin_fun1_t"),
    ._typeid = c->funtype1._typeid, // keep existing
    .result = type_uint,
    .params = { .v = params1, .len = countof(params1) },
  };
  typeid_intern((type_t*)&c->funtype1);

  // fun T.len() uint
  // Generated code is simply a constant or field access
  c->builtin_len = (fun_t){
    .kind = EXPR_FUN, .is_builtin = true, .flags = NF_VIS_PUB | NF_CHECKED, .nuse = 1,
    .type = (type_t*)&c->funtype1,
    .name = sym_len,
    .mangledname = (char*)(CO_ABI_GLOBAL_PREFIX "builtin_len"),
    .abi = ABI_C,
    .recvt = type_unknown,
  };

  // fun T.cap() uint
  // Generated code is simply a constant or field access
  c->builtin_cap = c->builtin_len;
  c->builtin_cap.name = sym_cap;
  c->builtin_cap.mangledname = (char*)(CO_ABI_GLOBAL_PREFIX "builtin_cap");
  safecheck(c->builtin_cap.mangledname);


  // [type] fun(mut this, uint cap) bool
  c->funtype2 = (funtype_t){
    .kind = TYPE_FUN,
    .is_builtin = true,
    .flags = NF_VIS_PUB | NF_CHECKED,
    .size = c->target.ptrsize,
    .align = c->target.ptrsize,
    .mangledname = (char*)(CO_ABI_GLOBAL_PREFIX "builtin_fun2_t"),
    ._typeid = c->funtype2._typeid, // keep existing
    .result = type_bool,
    .params = { .v = params2, .len = countof(params2) },
  };
  typeid_intern((type_t*)&c->funtype2);

  // fun [T].reserve(uint cap) bool
  // Generated code is a function call. Implementation in std/runtime.
  c->builtin_reserve = (fun_t){
    .kind = EXPR_FUN, .is_builtin = true, .flags = NF_VIS_PUB | NF_CHECKED, .nuse = 1,
    .type = (type_t*)&c->funtype2,
    .name = sym_len,
    .mangledname = (char*)(CO_ABI_GLOBAL_PREFIX "builtin_reserve"),
    .abi = ABI_C,
    .recvt = type_unknown,
  };

  // fun [T].resize(uint len) bool
  c->builtin_resize = c->builtin_reserve; // identical signature
  c->builtin_resize.name = sym_len;
  c->builtin_resize.mangledname = (char*)(CO_ABI_GLOBAL_PREFIX "builtin_resize");

  // Note: when adding or changing builtins, you should also update these functions:
  // - find_builtin_member in typecheck.c
  // - gen_call_builtin in cgen.c
}


static err_t configure_builtins(compiler_t* c) {
  configure_builtin_functions(c);

  if (c->builtins.cap > 0)
    map_dispose(&c->builtins, c->ma);

  const struct {
    sym_t       sym;
    const void* node;
  } entries[] = {
    // types
    {sym_void, type_void},
    {sym_bool, type_bool},
    {sym_int,  type_int},
    {sym_uint, type_uint},
    {sym_i8,   type_i8},
    {sym_i16,  type_i16},
    {sym_i32,  type_i32},
    {sym_i64,  type_i64},
    {sym_u8,   type_u8},
    {sym_u16,  type_u16},
    {sym_u32,  type_u32},
    {sym_u64,  type_u64},
    {sym_f32,  type_f32},
    {sym_f64,  type_f64},
    {sym_str,  &c->strtype},
  };

  if UNLIKELY(!map_init(&c->builtins, c->ma, countof(entries)))
    return ErrNoMem;

  for (usize i = 0; i < countof(entries); i++) {
    void** valp = map_assign_ptr(&c->builtins, c->ma, entries[i].sym);
    assertnotnull(valp);
    *valp = (void*)entries[i].node;
  }

  return 0;
}


err_t configure_options(compiler_t* c, const compiler_config_t* config) {
  c->buildmode = config->buildmode;
  c->opt_nolto = config->nolto;
  c->opt_nomain = config->nomain;
  c->opt_printast = config->printast;
  c->opt_printir = config->printir;
  c->opt_genirdot = config->genirdot;
  c->opt_genasm = config->genasm;
  c->opt_verbose = config->verbose;
  c->opt_nolibc = config->nolibc;
  c->opt_nolibcxx = config->nolibcxx;
  c->opt_nostdruntime = config->nostdruntime;
  return 0;
}


err_t configure_buildroot(compiler_t* c, const compiler_config_t* config) {
  mem_freecstr(c->ma, c->buildroot);
  if (!( c->buildroot = path_abs(config->buildroot).p ))
    return ErrNoMem;
  return 0;
}


err_t configure_builddir(compiler_t* c, const compiler_config_t* config) {
  // builddir = {buildroot}/{mode}-{target}

  char targetstr[TARGET_FMT_BUFCAP];
  target_fmt(&c->target, targetstr, sizeof(targetstr));
  slice_t target = slice_cstr(targetstr);

  slice_t mode = slice_cstr(buildmode_name(c->buildmode));

  usize len = strlen(c->buildroot) + 1 + mode.len;

  bool isnativetarget = strcmp(llvm_host_triple(), c->target.triple) == 0;
  if (!isnativetarget)
    len += target.len + 1;

  #define APPEND(slice)  memcpy(p, (slice).p, (slice).len), p += (slice.len)

  mem_freecstr(c->ma, c->builddir);

  c->builddir = mem_alloctv(c->ma, char, len + 1);
  if (!c->builddir)
    return ErrNoMem;

  char* p = c->builddir;
  APPEND(slice_cstr(c->buildroot));
  *p++ = PATH_SEPARATOR;
  APPEND(mode);
  if (!isnativetarget) {
    *p++ = '-';
    APPEND(target);
  }
  *p = 0;

  return 0;

  #undef APPEND
}


err_t compiler_configure(compiler_t* c, const compiler_config_t* config) {
  err_t err;
  err = configure_options(c, config);  if (err) return dlog("x"), err;
  err = configure_target(c, config);   if (err) return dlog("x"), err;
  err = configure_sysroot(c, config);  if (err) return dlog("x"), err;
  err = configure_buildroot(c, config);if (err) return dlog("x"), err;
  err = configure_builddir(c, config); if (err) return dlog("x"), err;
  err = configure_cflags(c, config);   if (err) return dlog("x"), err;
  err = configure_builtins(c);         if (err) return dlog("x"), err;
  return err;
}


//——————————————————————————————————————————————————————————————————————————————————————
// special packages


err_t compiler_get_runtime_pkg(compiler_t* c, pkg_t** rt_pkg) {
  // we cache the std/runtime package at compiler_t.stdruntime_pkg
  rwmutex_rlock(&c->pkgindex_mu);
  *rt_pkg = c->stdruntime_pkg;
  rwmutex_runlock(&c->pkgindex_mu);
  if (*rt_pkg) // found in cache
    return 0;

  err_t err;
  slice_t rt_pkgpath = slice_cstr("std/runtime");
  str_t rt_pkgdir = str_makelen(rt_pkgpath.chars, rt_pkgpath.len);
  usize rt_rootlen;

  // Resolve package
  // This will fail if it's not found on disk
  if UNLIKELY(( err = import_resolve_fspath(&rt_pkgdir, &rt_rootlen) )) {
    report_diag(c, (origin_t){0}, DIAG_ERR, "package std/runtime not found");
    goto end;
  }

  // check sanity of import_resolve_fspath
  assertf(
    strcmp(rt_pkgpath.chars, rt_pkgdir.p + rt_rootlen + 1) == 0,
    "import_resolve_fspath returned rootlen=%zu, dir='%s'",
    rt_rootlen, rt_pkgdir.p);

  // intern package in pkgindex
  // note: only possible error is ErrNoMem
  err = pkgindex_intern(
    c, str_slice(rt_pkgdir), rt_pkgpath, /*api_sha256*/NULL, rt_pkg);

end:
  str_free(rt_pkgdir);
  rwmutex_lock(&c->pkgindex_mu);
  // note: no race because of pkgindex_intern
  c->stdruntime_pkg = *rt_pkg;
  rwmutex_unlock(&c->pkgindex_mu);
  return err;
}


//——————————————————————————————————————————————————————————————————————————————————————
// name encoding


static bool fqn_recv(const compiler_t* c, buf_t* buf, const type_t* recv) {
  switch (recv->kind) {
    case TYPE_STRUCT:
      if (((structtype_t*)recv)->name)
        return buf_print(buf, ((structtype_t*)recv)->name);
      break;
    case TYPE_BOOL:
    case TYPE_I8:
    case TYPE_I16:
    case TYPE_I32:
    case TYPE_I64:
    case TYPE_INT:
    case TYPE_U8:
    case TYPE_U16:
    case TYPE_U32:
    case TYPE_U64:
    case TYPE_UINT:
    case TYPE_F32:
    case TYPE_F64:
      return buf_print(buf, primtype_name(recv->kind));
  }
  assertf(0,"TODO %s", nodekind_name(recv->kind));
  return buf_printf(buf, "TODO_%s_%s", __FUNCTION__, nodekind_name(recv->kind));
}


static bool fqn_fun(const compiler_t* c, const pkg_t* pkg, buf_t* buf, const fun_t* fn) {
  if (fn->abi == ABI_CO) {
    // mangle_str(buf, str_slice(pkg->path));
    // TODO: append name package was imported as, not its path
    buf_append(buf, pkg->path.p, pkg->path.len);
    buf_push(buf, '.');
    if (fn->recvt) {
      fqn_recv(c, buf, fn->recvt);
      buf_push(buf, '.');
    }
  }
  return buf_print(buf, fn->name);
}


bool compiler_fully_qualified_name(
  const compiler_t* c, const pkg_t* pkg, buf_t* buf, const node_t* n)
{
  // TODO: use n->nsparent when available
  if (n->kind == EXPR_FUN)
    return fqn_fun(c, pkg, buf, (fun_t*)n);
  assertf(0,"TODO global variable %s", nodekind_name(n->kind));
  return buf_printf(buf, "TODO_%s_%s", __FUNCTION__, nodekind_name(n->kind));
}


//————————————————————————————————————————————————————————————————————————————
// spawning tools as subprocesses, e.g. cc

// define SPAWN_TOOL_USE_FORK=1 to use fork() by default
#ifndef SPAWN_TOOL_USE_FORK
  #define SPAWN_TOOL_USE_FORK 1
  // // Note: In my (rsms) tests on macos x86, spawning new processes (posix_spawn)
  // // uses about 10x more memory than fork() in practice, likely from CoW.
  // #if defined(__APPLE__) && defined(DEBUG)
  //   // Note: fork() in debug builds on darwin are super slow, likely because of msan,
  //   // so we disable fork()ing in those cases.
  //   #define SPAWN_TOOL_USE_FORK 0
  // #else
  //   #define SPAWN_TOOL_USE_FORK 1
  // #endif
#endif


static err_t clang_fork(char*const* restrict argv) {
  int argc = 0;
  for (char*const* p = argv; *p++;)
    argc++;
  return clang_main(argc, argv) ? ErrCanceled : 0;
}


err_t spawn_tool(
  subproc_t* p, char*const* restrict argv, const char* nullable cwd, int flags)
{
  assert(argv[0] != NULL);

  // #if DEBUG
  // dlog("spawn_tool:");
  // printf(" ");
  // for (char*const* p = argv; *p; p++) printf(" '%s'", *p);
  // printf("\n");
  // #endif

  if (SPAWN_TOOL_USE_FORK && (flags & SPAWN_TOOL_NOFORK) == 0) {
    const char* cmd = argv[0];
    if (strcmp(cmd, "cc") == 0 || strcmp(cmd, "c++") == 0 ||
        strcmp(cmd, "clang") == 0 || strcmp(cmd, "clang++") == 0 ||
        strcmp(cmd, "as") == 0)
    {
      return subproc_fork(p, clang_fork, cwd, argv);
    }
  }
  return subproc_spawn(p, coexefile, argv, /*envp*/NULL, cwd);
}


err_t compiler_spawn_tool_p(
  const compiler_t* c, subproc_t* p, strlist_t* args, const char* nullable cwd)
{
  char* const* argv = strlist_array(args);
  if (!args->ok)
    return dlog("strlist_array failed"), ErrNoMem;
  int flags = 0;
  return spawn_tool(p, argv, cwd, flags);
}


err_t compiler_spawn_tool(
  const compiler_t* c, subprocs_t* procs, strlist_t* args, const char* nullable cwd)
{
  subproc_t* p = subprocs_alloc(procs);
  if (!p)
    return dlog("subprocs_alloc failed"), ErrCanceled;
  return compiler_spawn_tool_p(c, p, args, cwd);
}


err_t compiler_run_tool_sync(
  const compiler_t* c, strlist_t* args, const char* nullable cwd)
{
  subproc_t p = {0};
  err_t err = compiler_spawn_tool_p(c, &p, args, cwd);
  if (err)
    return err;
  return subproc_await(&p);
}


//————————————————————————————————————————————————————————————————————————————
// compiler_compile


static void add_cflags_for_srctype(compiler_t* c, strlist_t* args, filetype_t srctype) {
  switch ((enum filetype)srctype) {
    case FILE_C:     strlist_add_slice(args, c->cflags_c); break;
    case FILE_CO:    strlist_add_slice(args, c->cflags_co); break;
    case FILE_OTHER:
    case FILE_O:
      panic("unexpected srctype %u", srctype);
  }
}


static err_t cc_to_asm_main(
  compiler_t* c, const char* cfile, const char* asmfile, filetype_t srctype)
{
  strlist_t args = strlist_make(c->ma, "clang");
  add_cflags_for_srctype(c, &args, srctype);
  strlist_add(&args,
    "-w", // don't produce warnings (already reported by cc_to_obj_main)
    "-fno-lto", // make sure LTO is disabled or we will write LLVM IR
    "-S", "-xc", cfile,
    "-o", asmfile);
  char* const* argv = strlist_array(&args);
  if (!args.ok)
    return ErrNoMem;

  #if DEBUG
  dlog("cc %s -> %s", cfile, asmfile);
  if (c->opt_verbose > 1) {
    for (u32 i = 0; i < args.len; i++)
      fprintf(stderr, &" %s"[i==0], argv[i]);
    fprintf(stderr, "\n");
  }
  #endif

  int status = clang_main(args.len, argv);
  return status == 0 ? 0 : ErrCanceled;
}


static err_t cc_to_obj_main(
  compiler_t* c, const char* cfile, const char* ofile, filetype_t srctype)
{
  // note: clang crashes if we run it more than once in the same process

  strlist_t args = strlist_make(c->ma, "clang");
  add_cflags_for_srctype(c, &args, srctype);
  strlist_add(&args,
    // enable all warnings in debug builds, disable them in release builds
    #if DEBUG
      "-Wall",
      "-Wcovered-switch-default",
      "-Werror=implicit-function-declaration",
      "-Werror=incompatible-pointer-types",
      "-Werror=format-insufficient-args",
      "-Wno-unused-value",
      "-Wno-unused-function",
      "-Wno-tautological-compare" // e.g. "x == x"
    #else
      "-w"
    #endif
  );
  strlist_add(&args,
    "-c", "-xc", cfile,
    "-o", ofile,
    c->opt_verbose > 1 ? "-v" : "");

  char* const* argv = strlist_array(&args);
  if (!args.ok)
    return ErrNoMem;

  // dlog("cc %s -> %s", relpath(cfile), relpath(ofile));
  // if (c->opt_verbose > 1) {
  //   for (int i = 0; i < args.len; i++)
  //     fprintf(stderr, &" %s"[i==0], argv[i]);
  //   fprintf(stderr, "\n");
  // }

  int status = clang_main(args.len, argv);
  return status == 0 ? 0 : ErrCanceled;
}


err_t compile_c_to_obj_async(
  compiler_t* c,
  subprocs_t* sp,
  const char* wdir,
  const char* cfile,
  const char* ofile,
  filetype_t srctype)
{
  subproc_t* p = subprocs_alloc(sp);
  if (!p)
    return ErrNoMem;
  return subproc_fork(p, cc_to_obj_main, wdir, c, cfile, ofile, srctype);
}


err_t compile_c_to_asm_async(
  compiler_t* c,
  subprocs_t* sp,
  const char* wdir,
  const char* cfile,
  const char* ofile,
  filetype_t srctype)
{
  subproc_t* p = subprocs_alloc(sp);
  if (!p)
    return ErrNoMem;

  buf_t asmfile = buf_make(c->ma);
  buf_append(&asmfile, ofile, strlen(ofile) - 1); // FIXME: assumes ".c"
  buf_print(&asmfile, "S");
  if (!buf_nullterm(&asmfile))
    return ErrNoMem;

  return subproc_fork(p, cc_to_asm_main, wdir, c, cfile, asmfile.chars, srctype);
}
