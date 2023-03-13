This is the "COROOT" — files in this directory are used by compis at runtime and are bundled with the distribution (in the same directory as the compis executable, sans "lib/".)

Overview:

- `clangres` contains clang resources like SIMD headers.
  It is automatically included by compis for C source files and by the `cc` command.
- `co` contains headers used by compis C sources.
- `darwin` contains system library `.tbd` index files for macOS.
  These are used by the linker when targeting `macos`.
- `librt` contains the "builtins" runtime library,
  automatically built and included for compis and `cc` linking.
- `musl` contains sources and headers for musl, the Linux libc used for `linux` targets
- `sysinc` contains system headers.
  `sysinc` is not checked into git, but is generated from `sysinc-src`.
- `sysinc-src` is compiled to a more compact format as `sysinc`,
  which is what is actually included in distributions.
  During development, `build.sh` is responsible for building the `sysinc` as needed.
  (`sysinc` is not checked in to git since it is generated.)
- `wasi` contains sources for libc (and libwasi-\*) for the WASI platform.

Note: libc and librt are not used if `-nostdlib` is provided to `cc`.


## Updating `clangres`

    etc/update-clangres.sh


## Updating `librt`

    etc/update-librt.sh


## Updating `musl`

    etc/update-musl.sh

Note: public musl headers reside at `musl/include` rather than in `sysinc`.
It's separated like this since it's possible to compile for linux without libc.


## Updating `darwin` and `sysinc-src/*macos*`

You need to run this script on both an x86_64 mac and an arm one
since headers are resolved by clang.

Manual first steps:

1. Download all SDKs you'd like to import from
   <https://developer.apple.com/download/all/?q=command%20line>
   Note that you can search for specific OS versions, e.g.
   <https://developer.apple.com/download/all/?q=command%20line%2010.13>
2. Mount all downloaded disk images,
   so that you have e.g. "/Volumes/Command Line Developer Tools"
3. Run this script:

```
etc/update-macos-libs-and-headers.sh
```


## Updating `sysinc-src/*linux`

Needs to run on a Linux host (any distro, any arch)

    etc/update-linux-headers.sh


## Updating `wasi` and `sysinc-src/*wasi`

    etc/update-wasi.sh
