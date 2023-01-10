# First stage of a bootstrap build, inspired by
# clang/cmake/caches/DistributionExample.cmake.

# Enable LLVM projects
#
# We don't use LLVM_ENABLE_RUNTIMES here because it causes issues with clang
# bootstrap. See the note in build_llvm.sh on why it's still fine to build
# libc++ and friends as regular projects.
set(LLVM_ENABLE_PROJECTS "clang;clang-tools-extra;lld;libcxx;libcxxabi;compiler-rt;libunwind" CACHE STRING "")

# Only build the native target in stage1 since it is a throwaway build.
set(LLVM_TARGETS_TO_BUILD Native CACHE STRING "")

# Optimize the stage1 compiler, but don't LTO it because that wastes time.
set(CMAKE_BUILD_TYPE Release CACHE STRING "")

# Setup vendor-specific settings.
set(PACKAGE_VENDOR "rsms" CACHE STRING "")

# Setting up the stage2 LTO option needs to be done on the stage1 build so that
# the proper LTO library dependencies can be connected.
set(BOOTSTRAP_LLVM_ENABLE_LTO ON CACHE BOOL "")

if (NOT APPLE)
  # Since LLVM_ENABLE_LTO is ON we need a LTO capable linker
  set(BOOTSTRAP_LLVM_ENABLE_LLD ON CACHE BOOL "")
endif()

# # Avoid running out of memory
# set(LLVM_PARALLEL_LINK_JOBS "1" CACHE STRING "")

# # Allow old gcc from CentOS 6 for stage 1 build
# set(LLVM_TEMPORARILY_ALLOW_OLD_TOOLCHAIN ON CACHE BOOL "")

# Expose stage2 targets through the stage1 build configuration.
set(CLANG_BOOTSTRAP_TARGETS
  check-all
  check-llvm
  check-clang
  llvm-config
  test-suite
  test-depends
  llvm-test-depends
  clang-test-depends
  distribution
  install-distribution
  clang CACHE STRING "")

# Setup the bootstrap build.
set(CLANG_ENABLE_BOOTSTRAP ON CACHE BOOL "")

if(STAGE2_CACHE_FILE)
  set(CLANG_BOOTSTRAP_CMAKE_ARGS
    -C ${STAGE2_CACHE_FILE}
    CACHE STRING "")
else()
  set(CLANG_BOOTSTRAP_CMAKE_ARGS
    -C ${CMAKE_CURRENT_LIST_DIR}/stage2.cmake
    CACHE STRING "")
endif()
