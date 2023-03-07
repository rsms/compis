// SPDX-License-Identifier: Apache-2.0
#include "colib.h"
#include <errno.h>


const char* err_str(err_t e) {
  switch ((enum err_)e) {
  case ErrOk:           return "(no error)";
  case ErrInvalid:      return "invalid data or argument";
  case ErrSysOp:        return "invalid syscall op or syscall op data";
  case ErrBadfd:        return "invalid file descriptor";
  case ErrBadName:      return "invalid or misformed name";
  case ErrNotFound:     return "not found";
  case ErrNameTooLong:  return "name too long";
  case ErrCanceled:     return "operation canceled";
  case ErrNotSupported: return "not supported";
  case ErrExists:       return "already exists";
  case ErrEnd:          return "end of resource";
  case ErrAccess:       return "permission denied";
  case ErrNoMem:        return "cannot allocate memory";
  case ErrMFault:       return "bad memory address";
  case ErrOverflow:     return "value too large";
  case ErrReadOnly:     return "read-only";
  case ErrIO:           return "I/O error";
  case ErrNotDir:       return "not a directory";
  }
  return "(unknown error)";
}


err_t err_errnox(int e) {
  switch (e) {
    case 0: return 0;
    case EACCES:  return ErrAccess;
    case EEXIST:  return ErrExists;
    case ENOENT:  return ErrNotFound;
    case EBADF:   return ErrBadfd;
    case EROFS:   return ErrReadOnly;
    case EIO:     return ErrIO;
    case ENOTDIR: return ErrNotDir;
    case ENOTSUP:
    case ENOSYS:
      return ErrNotSupported;
    default: return ErrInvalid;
  }
}


err_t err_errno() {
  return err_errnox(errno);
}
