# Second stage of a bootstrap build, inspired by
# clang/cmake/caches/DistributionExample-stage2.cmake.

set(LLVM_ENABLE_PROJECTS "clang;clang-tools-extra;lld;libcxx;libcxxabi;compiler-rt;libunwind" CACHE STRING "")

# We only need x86 (which includes x86_64)
set(LLVM_TARGETS_TO_BUILD X86 CACHE STRING "")
# set(LLVM_TARGETS_TO_BUILD AArch64;ARM;Mips;RISCV;WebAssembly;X86 CACHE STRING "")

set(CMAKE_BUILD_TYPE Release CACHE STRING "")

# Use libc++ and libc++abi for the stage-2 artifacts.
#
# A more elegant approach would be to make stage-1 clang use libc++ by default
# (via CLANG_DEFAULT_CXX_STDLIB), but this causes problems when building LLVM
# passes (e.g., in AFL++): if the build system of the pass uses llvm-config to
# get the required compiler flags, there will be no mention of libc++, resulting
# in linker errors when we try to load the pass later on. This could be resolved
# by making also stage-2 clang link against libc++ by default, but then we might
# surprise users who expect libstdc++ to be used by default.
set(CMAKE_CXX_FLAGS "-stdlib=libc++" CACHE STRING "")
set(CMAKE_SHARED_LINKER_FLAGS "-lc++ -lc++abi" CACHE STRING "")
set(CMAKE_MODULE_LINKER_FLAGS "-lc++ -lc++abi" CACHE STRING "")
set(CMAKE_EXE_LINKER_FLAGS "-lc++ -lc++abi" CACHE STRING "")

set(SANITIZER_CXX_ABI "libc++" CACHE STRING "")
set(SANITIZER_TEST_CXX "libc++" CACHE STRING "")

# Build a hermetic version of libc++. This means that the static library will
# not export any symbols, so we can link it into our shared objects without
# reexporting libc++ symbols to user code (which may collide with symbols
# defined there).
# set(LIBCXX_HERMETIC_STATIC_LIBRARY ON CACHE BOOL "")

# # Avoid running out of memory
# set(LLVM_PARALLEL_LINK_JOBS "1" CACHE STRING "")

set(PACKAGE_VENDOR "rsms" CACHE STRING "")

# # Create libLLVM.so in addition to the static libraries
# set(LLVM_BUILD_LLVM_DYLIB ON CACHE BOOL "")

# When LLVM_INSTALL_TOOLCHAIN_ONLY is set, LLVM only installs the tools
# specified in this list. We don't set the option (because we also need LLVM
# development files), but let's keep the tools in a separate list nonetheless in
# case we decide to switch later on.
set(LLVM_TOOLCHAIN_TOOLS
  dsymutil
  llvm-ar
  llvm-as
  llvm-cov
  llvm-config
  llvm-dwarfdump
  llvm-profdata
  llvm-objdump
  llvm-objcopy
  llvm-nm
  llvm-size
  llvm-strip
  llvm-dwp
  llvm-symbolizer
  CACHE STRING "")

set(LLVM_DISTRIBUTION_COMPONENTS
  clang
  clang-headers
  clang-libraries
  lld
  LTO
  clang-apply-replacements
  clang-format
  clang-tidy
  clang-resource-headers
  builtins
  llvm-headers
  llvm-libraries
  cxx
  cxx-headers
  cxxabi
  compiler-rt
  compiler-rt-headers
  unwind
  cmake-exports
  ${LLVM_TOOLCHAIN_TOOLS}
  CACHE STRING "")
