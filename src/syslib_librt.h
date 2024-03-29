// librt sources
// Do not edit! Generated by etc/update-librt.sh
// SPDX-License-Identifier: Apache-2.0
static const char* const librt_sources[] = {
  "absvdi2.c",
  "absvsi2.c",
  "absvti2.c",
  "adddf3.c",
  "addsf3.c",
  "addvdi3.c",
  "addvsi3.c",
  "addvti3.c",
  "apple_versioning.c",
  "ashldi3.c",
  "ashlti3.c",
  "ashrdi3.c",
  "ashrti3.c",
  "bswapdi2.c",
  "bswapsi2.c",
  "clzdi2.c",
  "clzsi2.c",
  "clzti2.c",
  "cmpdi2.c",
  "cmpti2.c",
  "comparedf2.c",
  "comparesf2.c",
  "ctzdi2.c",
  "ctzsi2.c",
  "ctzti2.c",
  "divdc3.c",
  "divdf3.c",
  "divdi3.c",
  "divmoddi4.c",
  "divmodsi4.c",
  "divmodti4.c",
  "divsc3.c",
  "divsf3.c",
  "divsi3.c",
  "divti3.c",
  "extendsfdf2.c",
  "extendhfsf2.c",
  "ffsdi2.c",
  "ffssi2.c",
  "ffsti2.c",
  "fixdfdi.c",
  "fixdfsi.c",
  "fixdfti.c",
  "fixsfdi.c",
  "fixsfsi.c",
  "fixsfti.c",
  "fixunsdfdi.c",
  "fixunsdfsi.c",
  "fixunsdfti.c",
  "fixunssfdi.c",
  "fixunssfsi.c",
  "fixunssfti.c",
  "floatdidf.c",
  "floatdisf.c",
  "floatsidf.c",
  "floatsisf.c",
  "floattidf.c",
  "floattisf.c",
  "floatundidf.c",
  "floatundisf.c",
  "floatunsidf.c",
  "floatunsisf.c",
  "floatuntidf.c",
  "floatuntisf.c",
  "fp_mode.c",
  "int_util.c",
  "lshrdi3.c",
  "lshrti3.c",
  "moddi3.c",
  "modsi3.c",
  "modti3.c",
  "muldc3.c",
  "muldf3.c",
  "muldi3.c",
  "mulodi4.c",
  "mulosi4.c",
  "muloti4.c",
  "mulsc3.c",
  "mulsf3.c",
  "multi3.c",
  "mulvdi3.c",
  "mulvsi3.c",
  "mulvti3.c",
  "negdf2.c",
  "negdi2.c",
  "negsf2.c",
  "negti2.c",
  "negvdi2.c",
  "negvsi2.c",
  "negvti2.c",
  "os_version_check.c",
  "paritydi2.c",
  "paritysi2.c",
  "parityti2.c",
  "popcountdi2.c",
  "popcountsi2.c",
  "popcountti2.c",
  "powidf2.c",
  "powisf2.c",
  "subdf3.c",
  "subsf3.c",
  "subvdi3.c",
  "subvsi3.c",
  "subvti3.c",
  "trampoline_setup.c",
  "truncdfhf2.c",
  "truncdfsf2.c",
  "truncsfhf2.c",
  "ucmpdi2.c",
  "ucmpti2.c",
  "udivdi3.c",
  "udivmoddi4.c",
  "udivmodsi4.c",
  "udivmodti4.c",
  "udivsi3.c",
  "udivti3.c",
  "umoddi3.c",
  "umodsi3.c",
  "umodti3.c",
  "addtf3.c",
  "comparetf2.c",
  "divtc3.c",
  "divtf3.c",
  "extenddftf2.c",
  "extendhftf2.c",
  "extendsftf2.c",
  "fixtfdi.c",
  "fixtfsi.c",
  "fixtfti.c",
  "fixunstfdi.c",
  "fixunstfsi.c",
  "fixunstfti.c",
  "floatditf.c",
  "floatsitf.c",
  "floattitf.c",
  "floatunditf.c",
  "floatunsitf.c",
  "floatuntitf.c",
  "multc3.c",
  "multf3.c",
  "powitf2.c",
  "subtf3.c",
  "trunctfdf2.c",
  "trunctfhf2.c",
  "trunctfsf2.c",
  "clear_cache.c",
  "aarch64/cpu_model.c",
  "aarch64/fp_mode.c",
  "aarch64/lse.S",
  "arm/fp_mode.c",
  "arm/bswapdi2.S",
  "arm/bswapsi2.S",
  "arm/clzdi2.S",
  "arm/clzsi2.S",
  "arm/comparesf2.S",
  "arm/divmodsi4.S",
  "arm/divsi3.S",
  "arm/modsi3.S",
  "arm/sync_fetch_and_add_4.S",
  "arm/sync_fetch_and_add_8.S",
  "arm/sync_fetch_and_and_4.S",
  "arm/sync_fetch_and_and_8.S",
  "arm/sync_fetch_and_max_4.S",
  "arm/sync_fetch_and_max_8.S",
  "arm/sync_fetch_and_min_4.S",
  "arm/sync_fetch_and_min_8.S",
  "arm/sync_fetch_and_nand_4.S",
  "arm/sync_fetch_and_nand_8.S",
  "arm/sync_fetch_and_or_4.S",
  "arm/sync_fetch_and_or_8.S",
  "arm/sync_fetch_and_sub_4.S",
  "arm/sync_fetch_and_sub_8.S",
  "arm/sync_fetch_and_umax_4.S",
  "arm/sync_fetch_and_umax_8.S",
  "arm/sync_fetch_and_umin_4.S",
  "arm/sync_fetch_and_umin_8.S",
  "arm/sync_fetch_and_xor_4.S",
  "arm/sync_fetch_and_xor_8.S",
  "arm/udivmodsi4.S",
  "arm/udivsi3.S",
  "arm/umodsi3.S",
  "arm_eabi/aeabi_cdcmp.S",
  "arm_eabi/aeabi_cdcmpeq_check_nan.c",
  "arm_eabi/aeabi_cfcmp.S",
  "arm_eabi/aeabi_cfcmpeq_check_nan.c",
  "arm_eabi/aeabi_dcmp.S",
  "arm_eabi/aeabi_div0.c",
  "arm_eabi/aeabi_drsub.c",
  "arm_eabi/aeabi_fcmp.S",
  "arm_eabi/aeabi_frsub.c",
  "arm_eabi/aeabi_idivmod.S",
  "arm_eabi/aeabi_ldivmod.S",
  "arm_eabi/aeabi_memcmp.S",
  "arm_eabi/aeabi_memcpy.S",
  "arm_eabi/aeabi_memmove.S",
  "arm_eabi/aeabi_memset.S",
  "arm_eabi/aeabi_uidivmod.S",
  "arm_eabi/aeabi_uldivmod.S",
  "x86/cpu_model.c",
  "x86/fp_mode.c",
  "i386/ashldi3.S",
  "i386/ashrdi3.S",
  "i386/divdi3.S",
  "i386/floatdidf.S",
  "i386/floatdisf.S",
  "i386/floatundidf.S",
  "i386/floatundisf.S",
  "i386/lshrdi3.S",
  "i386/moddi3.S",
  "i386/muldi3.S",
  "i386/udivdi3.S",
  "i386/umoddi3.S",
  "i386/floatdixf.S",
  "i386/floatundixf.S",
  "i386/divxc3.c",
  "i386/fixxfdi.c",
  "i386/fixxfti.c",
  "i386/fixunsxfdi.c",
  "i386/fixunsxfsi.c",
  "i386/fixunsxfti.c",
  "i386/floatdixf.c",
  "i386/floattixf.c",
  "i386/floatundixf.c",
  "i386/floatuntixf.c",
  "i386/mulxc3.c",
  "i386/powixf2.c",
  "x86_64/floatdidf.c",
  "x86_64/floatdisf.c",
  "x86_64/floatundidf.S",
  "x86_64/floatundisf.S",
  "x86_64/floatdixf.c",
  "x86_64/floatundixf.S",
  "x86_64/divxc3.c",
  "x86_64/fixxfdi.c",
  "x86_64/fixxfti.c",
  "x86_64/fixunsxfdi.c",
  "x86_64/fixunsxfsi.c",
  "x86_64/fixunsxfti.c",
  "x86_64/floatdixf.c",
  "x86_64/floattixf.c",
  "x86_64/floatundixf.c",
  "x86_64/floatuntixf.c",
  "x86_64/mulxc3.c",
  "x86_64/powixf2.c",
  "riscv/save.S",
  "riscv/restore.S",
  "riscv32/mulsi3.S",
  "riscv64/muldi3.S",
  "any-macos/atomic_flag_clear.c",
  "any-macos/atomic_flag_clear_explicit.c",
  "any-macos/atomic_flag_test_and_set.c",
  "any-macos/atomic_flag_test_and_set_explicit.c",
  "any-macos/atomic_signal_fence.c",
  "any-macos/atomic_thread_fence.c",
};

typedef struct {
  targetdesc_t target;
  u8 sources[32]; // bitmap index into librt_sources
} librt_srclist_t;
static const librt_srclist_t librt_srclist[] = {
  {{ARCH_aarch64, SYS_linux, ""}, {0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0x1f}},
  {{ARCH_arm, SYS_linux, ""}, {0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xe3,0xff,0xff,0xff,0x1f}},
  {{ARCH_i386, SYS_linux, ""}, {0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0x3,0,0,0,0,0,0,0xff,0xff,0xff,0x3}},
  {{ARCH_riscv64, SYS_linux, ""}, {0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0x3,0,0,0,0,0,0,0,0,0,0,0,0x80}},
  {{ARCH_x86_64, SYS_linux, ""}, {0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0x3,0,0,0,0,0,0,0,0,0,0xbc,0xff,0xf}},
  {{ARCH_aarch64, SYS_macos, "11"}, {0xff,0xfe,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xfe,0x7f,0xfb,0xff,0xc7,0x1f,0,0,0,0,0,0,0,0,0,0,0,0,0x3f}},
  {{ARCH_aarch64, SYS_macos, "12"}, {0xff,0xfe,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xfe,0x7f,0xfb,0xff,0xc7,0x1f,0,0,0,0,0,0,0,0,0,0,0,0,0x3f}},
  {{ARCH_aarch64, SYS_macos, "13"}, {0xff,0xfe,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xfe,0x7f,0xfb,0xff,0xc7,0x1f,0,0,0,0,0,0,0,0,0,0,0,0,0x3f}},
  {{ARCH_x86_64, SYS_macos, "10"}, {0xff,0xfe,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xfe,0x7f,0xfb,0xff,0xc7,0x3,0,0,0,0,0,0,0,0,0,0xbc,0xff,0xf,0x3f}},
  {{ARCH_x86_64, SYS_macos, "11"}, {0xff,0xfe,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xfe,0x7f,0xfb,0xff,0xc7,0x3,0,0,0,0,0,0,0,0,0,0xbc,0xff,0xf,0x3f}},
  {{ARCH_x86_64, SYS_macos, "12"}, {0xff,0xfe,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xfe,0x7f,0xfb,0xff,0xc7,0x3,0,0,0,0,0,0,0,0,0,0xbc,0xff,0xf,0x3f}},
  {{ARCH_x86_64, SYS_macos, "13"}, {0xff,0xfe,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xfe,0x7f,0xfb,0xff,0xc7,0x3,0,0,0,0,0,0,0,0,0,0xbc,0xff,0xf,0x3f}},
  {{ARCH_wasm32, SYS_wasi, ""}, {0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0x3}},
  {{ARCH_wasm32, SYS_none, ""}, {0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0x3}},
  {{ARCH_wasm64, SYS_none, ""}, {0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0x3}},
};
