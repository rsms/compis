// Simple tar extractor with limited functionality.
// Just enough to extract tar files bundled with compis.
// Based on public-domain work "untar.c" by Tim Kientzle, March 2009.
// SPDX-License-Identifier: Apache-2.0
#include "colib.h"
#include "path.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h> // mkdir
#include <unistd.h> // getcwd
#include <errno.h>
#if defined(_WIN32) && !defined(__CYGWIN__)
  #include <windows.h>
#endif


static int parseoct(const char *p, usize n) {
  // Parse an octal number, ignoring leading and trailing nonsense
  int i = 0;
  while ((*p < '0' || *p > '7') && n > 0) {
    ++p;
    --n;
  }
  while (*p >= '0' && *p <= '7' && n > 0) {
    i *= 8;
    i += *p - '0';
    ++p;
    --n;
  }
  return (i);
}


static bool is_end_of_archive(const char* p) {
  // end of tar is a 512 chunk of just zeroes
  const u64 z[512/sizeof(u64)];
  return memcmp(p, z, 512) == 0;
}


static bool verify_checksum(const char *p) {
  int u = 0;
  for (int n = 0; n < 512; ++n) {
    if (n < 148 || n > 155) {
      // Standard tar checksum adds unsigned bytes
      u += ((unsigned char *)p)[n];
    } else {
      u += 0x20;
    }
  }
  return u == parseoct(p + 148, 8);
}


static err_t create_file(memalloc_t ma, FILE** fp, char* path, int mode) {
  if (( *fp = fopen(path, "wb+") ))
    return 0;
  if (errno != ENOENT)
    return err_errno();
  // try creating directory
  char* dir = path_dir_m(ma, path);
  if (!dir)
    return ErrNoMem;
  err_t err;
  if (*dir == 0) {
    err = ErrNotFound;
  } else {
    if (!( err = fs_mkdirs_verbose(dir, 0755) )) {
      if ( ( *fp = fopen(path, "wb+") ) == NULL)
        err = err_errno();
    }
  }
  mem_freecstr(ma, dir);
  return err;
}


#define vlog(fmt, args...) \
  (coverbose && printf(fmt "\n", ##args))
  // (coverbose && printf("%s: " fmt "\n", relpath(tarfile), ##args))


static err_t tar_extract1(
  memalloc_t ma,
  const char* restrict tarfile,
  FILE* tarf,
  char* restrict buf,
  char* restrict dstpath,
  char* restrict dstpath2,
  usize dstdirlen)
{
  FILE* f = NULL;
  usize bytes_read, namelen, linklen;
  int filesize;
  err_t err = 0;

  #define VALIDATE_FILENAME(start) ({ \
    usize len__ = strnlen((start), 100); \
    if UNLIKELY(len__ == 100) { \
      vlog("invalid filename"); \
      return ErrInvalid; \
    } \
    len__; \
  })

  // basic tar entry:
  //   Offset  Size  Data
  //        0   100   File name
  //      100     8   File mode (octal)
  //      108     8   Owner's numeric user ID (octal)
  //      116     8   Group's numeric user ID (octal)
  //      124    12   File size in bytes (octal)
  //      136    12   Last modification time in numeric Unix time format (octal)
  //      148     8   Checksum for header record
  //
  // classic tar entry:
  //   Offset  Size  Data
  //        0   156  <basic tar entry>
  //      156     1  Link indicator (file type)
  //      157   100  Name of linked file
  //   Link indicator:
  //     '0' (or 0x0) Normal file
  //     '1' Hard link
  //     '2' Symbolic link
  //
  // ustar entry:
  //   Offset  Size  Data
  //        0   156  <basic tar entry>
  //      156     1  Type flag
  //      157   100  Name of linked file (same as "classic tar")
  //      257     6  UStar indicator, "ustar", then NUL
  //      263     2  UStar version, "00"
  //      265    32  Owner user name
  //      297    32  Owner group name
  //      329     8  Device major number
  //      337     8  Device minor number
  //      345   155  Filename prefix
  //   Type flag:
  //     '0' (or 0x0) Normal file
  //     '1' Hard link
  //     '2' Symbolic link
  //     '3' Character special
  //     '4' Block special
  //     '5' Directory
  //     '6' FIFO
  //     '7' Contiguous file
  //     'g' Global extended header with meta data (POSIX.1-2001)
  //     'x' Extended header w/ metadata for the next file in the archive (POSIX.1-2001)
  //     'A'–'Z' Vendor specific extensions (POSIX.1-1988)
  //
  //
  for (;;) { // for each entry
    bytes_read = fread(buf, 1, 512, tarf);
    if UNLIKELY(bytes_read < 512) {
      dlog("short read on %s: expected 512, got %zu\n", tarfile, bytes_read);
      vlog("corrupt tar data");
      return ErrIO;
    }

    if (*(unsigned long*)buf == 0lu && is_end_of_archive(buf)) {
      dlog("end of %s", tarfile);
      return 0;
    }

    if UNLIKELY(!verify_checksum(buf)) {
      vlog("checksum failure");
      return ErrInvalid;
    }

    namelen = VALIDATE_FILENAME(buf);
    memcpy(&dstpath[dstdirlen], buf, namelen + 1);

    filesize = parseoct(buf + 124, 12);

    switch (buf[156]) {
      case '1':
        linklen = VALIDATE_FILENAME(&buf[157]);
        memcpy(&dstpath2[dstdirlen], &buf[157], linklen + 1);
        if (link(dstpath2, dstpath) != 0) {
          if UNLIKELY(
            errno != EEXIST ||
            (unlink(dstpath) != 0 && errno != ENOENT) ||
            link(dstpath2, dstpath) != 0
          ) {
            vlog("failed to create hardlink %s: %s", dstpath, strerror(errno));
            return err_errno();
          }
        }
        vlog("create hardlink %s -> %s", buf, &buf[157]);
        filesize = 0;
        break;
      case '2':
        vlog("symlink unsupported %s", buf);
        return ErrNotSupported;
      case '3':
        vlog("character device unsupported %s", buf);
        return ErrNotSupported;
      case '4':
        vlog("block device unsupported %s", buf);
        return ErrNotSupported;
      case '6':
        vlog("FIFO unsupported %s", buf);
        return ErrNotSupported;
      case '5':
        vlog("create directory %s", buf);
        if (( err = fs_mkdirs(dstpath, parseoct(buf + 100, 8)) ))
          return err;
        filesize = 0;
        break;
      case 0x0:
      case '0':
      case '7':
        vlog("create file %s", buf);
        if (( err = create_file(ma, &f, dstpath, parseoct(buf + 100, 8)) ))
          return err;
        break;
      default:
        dlog("ignoring entry field '%c'", buf[156]);
        break;
    }

    while (filesize > 0) {
      bytes_read = fread(buf, 1, 512, tarf);
      if (bytes_read < 512) {
        dlog("short read on %s: Expected 512, got %zu\n", tarfile, bytes_read);
        vlog("corrupt tar data");
        fclose(f);
        return ErrIO;
      }
      if (filesize < 512)
        bytes_read = filesize;
      if (f != NULL) {
        if (fwrite(buf, 1, bytes_read, f) != bytes_read) {
          vlog("write error: %s", strerror(errno));
          fclose(f);
          return ErrIO;
        }
      }
      filesize -= bytes_read;
    }

    if (f) {
      fclose(f);
      f = NULL;
    }
  }
}


err_t tar_extract(memalloc_t ma, const char* tarfile, const char* dstdir) {
  err_t err;

  #if DEBUG
  if (!coverbose)
    dlog("extract %s -> %s", relpath(tarfile), relpath(dstdir));
  #endif
  vlog("extract %s -> %s", relpath(tarfile), relpath(dstdir));

  usize dstdirlen = strlen(dstdir);
  if (dstdirlen > PATH_MAX - 100) {
    vlog("dstdir too long: %s", dstdir);
    return ErrOverflow;
  }

  FILE* f = fopen(tarfile, "rb");
  if (!f) {
    err = err_errno();
    vlog("%s", err_str(err));
    return err;
  }

  mem_t m = mem_alloc(ma, 512 + (dstdirlen + 100 + 1)*2);
  char* dstpath = m.p + 512;
  char* dstpath2 = m.p + 512 + dstdirlen + 100 + 1;
  memcpy(dstpath, dstdir, dstdirlen);
  dstpath[dstdirlen++] = PATH_SEPARATOR;
  memcpy(dstpath2, dstpath, dstdirlen);

  err = tar_extract1(ma, tarfile, f, m.p, dstpath, dstpath2, dstdirlen);

  fclose(f);

  return err;
}
