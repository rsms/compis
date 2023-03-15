#!/usr/bin/env bash
set -euo pipefail
source "$(dirname "$0")/lib.sh"
cd "$PROJECT"

WASI_SRCDIR="$DEPS_DIR/wasi"
HEADER_DESTDIR="$PROJECT/lib/sysinc-src/wasm32-wasi"
SOURCE_DESTDIR="$PROJECT/lib/wasi"
INFO_H_FILE="$PROJECT/src/syslib_wasi.h"
THREAD_MODEL=single  # "single" or "posix"(WIP)

if [ -d "$WASI_SRCDIR" ]; then
  echo git -C "$WASI_SRCDIR" pull origin main
       git -C "$WASI_SRCDIR" pull origin main
else
  git clone https://github.com/WebAssembly/wasi-libc "$WASI_SRCDIR"
fi

_pushd "$WASI_SRCDIR"

# ————————————————————————————————————————————————————————————————————————————————————
# Copy headers (see "include_dirs" in wasi's Makefile)

MUSL_OMIT_HEADERS=()

# Remove files which aren't headers (we generate alltypes.h below).
MUSL_OMIT_HEADERS+=(
  "bits/alltypes.h.in" \
  "alltypes.h.in" \
)
# Use the compiler's version of these headers.
MUSL_OMIT_HEADERS+=(
  "stdarg.h" \
  "stddef.h" \
)
# Use the WASI errno definitions.
MUSL_OMIT_HEADERS+=(
  "bits/errno.h" \
)
# Remove headers that aren't supported yet or that aren't relevant for WASI.
MUSL_OMIT_HEADERS+=(
  "sys/procfs.h" \
  "sys/user.h" \
  "sys/kd.h" "sys/vt.h" "sys/soundcard.h" "sys/sem.h" \
  "sys/shm.h" "sys/msg.h" "sys/ipc.h" "sys/ptrace.h" \
  "sys/statfs.h" \
  "bits/kd.h" "bits/vt.h" "bits/soundcard.h" "bits/sem.h" \
  "bits/shm.h" "bits/msg.h" "bits/ipc.h" "bits/ptrace.h" \
  "bits/statfs.h" \
  "sys/vfs.h" \
  "sys/statvfs.h" \
  "syslog.h" "sys/syslog.h" \
  "wait.h" "sys/wait.h" \
  "ucontext.h" "sys/ucontext.h" \
  "paths.h" \
  "utmp.h" "utmpx.h" \
  "lastlog.h" \
  "sys/acct.h" \
  "sys/cachectl.h" \
  "sys/epoll.h" "sys/reboot.h" "sys/swap.h" \
  "sys/sendfile.h" "sys/inotify.h" \
  "sys/quota.h" \
  "sys/klog.h" \
  "sys/fsuid.h" \
  "sys/io.h" \
  "sys/prctl.h" \
  "sys/mtio.h" \
  "sys/mount.h" \
  "sys/fanotify.h" \
  "sys/personality.h" \
  "elf.h" "link.h" "bits/link.h" \
  "scsi/scsi.h" "scsi/scsi_ioctl.h" "scsi/sg.h" \
  "sys/auxv.h" \
  "pwd.h" "shadow.h" "grp.h" \
  "mntent.h" \
  "netdb.h" \
  "resolv.h" \
  "pty.h" \
  "dlfcn.h" \
  "setjmp.h" \
  "ulimit.h" \
  "sys/xattr.h" \
  "wordexp.h" \
  "spawn.h" \
  "sys/membarrier.h" \
  "sys/signalfd.h" \
  "termios.h" \
  "sys/termios.h" \
  "bits/termios.h" \
  "net/if.h" \
  "net/if_arp.h" \
  "net/ethernet.h" \
  "net/route.h" \
  "netinet/if_ether.h" \
  "netinet/ether.h" \
  "sys/timerfd.h" \
  "libintl.h" \
  "sys/sysmacros.h" \
  "aio.h" \
)
[ $THREAD_MODEL = single ] &&
  MUSL_OMIT_HEADERS+=( pthread.h )

rm -rf "$HEADER_DESTDIR"
mkdir -p "$(dirname "$HEADER_DESTDIR")"

cp -r libc-bottom-half/headers/public        "$HEADER_DESTDIR"
cp -r libc-top-half/musl/include/*           "$HEADER_DESTDIR/"
cp -r libc-top-half/musl/arch/generic/bits   "$HEADER_DESTDIR/"
cp -r libc-top-half/musl/arch/wasm32/bits/*  "$HEADER_DESTDIR/bits/"
sed -f libc-top-half/musl/tools/mkalltypes.sed \
       libc-top-half/musl/arch/wasm32/bits/alltypes.h.in \
       libc-top-half/musl/include/alltypes.h.in \
       > "$HEADER_DESTDIR/bits/alltypes.h"

rm "${MUSL_OMIT_HEADERS[@]/#/$HEADER_DESTDIR/}"
find "$HEADER_DESTDIR" -type f -name 'README*' -delete
find "$HEADER_DESTDIR" -empty -type d -delete

# ————————————————————————————————————————————————————————————————————————————————————
# Copy sources

# make-compatible "filter-out" function
filter-out() { # <exclude>... "," <file>...
  local name skip exf
  local excl=()
  while [[ $# -gt 0 ]]; do
    if [[ "$1" == *"," ]]; then
      if [ "$1" != "," ]; then
        excl+=( ${1:0:$(( ${#1} - 1 ))} )
      fi
      shift
      break
    fi
    excl+=( $1 ) ; shift
  done
  # echo "excl=${excl[@]}" >&2
  for f in "$@"; do
    skip=
    for exf in "${excl[@]}"; do
      if [ ${exf:0:1} = '%' ]; then
        if [[ "$f" == *"${exf:1}" ]]; then
          skip=1
          break
        fi
      elif [ "$f" = "$exf" ]; then
        skip=1
        break
      fi
    done
    [ -n "$skip" ] || echo "$f"
    # [ -z "$skip" ] || echo "[skip] $f" >&2
  done
}

LIBC_TOP_HALF_MUSL_SOURCES=(
  libc-top-half/musl/src/misc/a64l.c \
  libc-top-half/musl/src/misc/basename.c \
  libc-top-half/musl/src/misc/dirname.c \
  libc-top-half/musl/src/misc/ffs.c \
  libc-top-half/musl/src/misc/ffsl.c \
  libc-top-half/musl/src/misc/ffsll.c \
  libc-top-half/musl/src/misc/fmtmsg.c \
  libc-top-half/musl/src/misc/getdomainname.c \
  libc-top-half/musl/src/misc/gethostid.c \
  libc-top-half/musl/src/misc/getopt.c \
  libc-top-half/musl/src/misc/getopt_long.c \
  libc-top-half/musl/src/misc/getsubopt.c \
  libc-top-half/musl/src/misc/uname.c \
  libc-top-half/musl/src/misc/nftw.c \
  libc-top-half/musl/src/errno/strerror.c \
  libc-top-half/musl/src/network/htonl.c \
  libc-top-half/musl/src/network/htons.c \
  libc-top-half/musl/src/network/ntohl.c \
  libc-top-half/musl/src/network/ntohs.c \
  libc-top-half/musl/src/network/inet_ntop.c \
  libc-top-half/musl/src/network/inet_pton.c \
  libc-top-half/musl/src/network/inet_aton.c \
  libc-top-half/musl/src/network/in6addr_any.c \
  libc-top-half/musl/src/network/in6addr_loopback.c \
  libc-top-half/musl/src/fenv/fenv.c \
  libc-top-half/musl/src/fenv/fesetround.c \
  libc-top-half/musl/src/fenv/feupdateenv.c \
  libc-top-half/musl/src/fenv/fesetexceptflag.c \
  libc-top-half/musl/src/fenv/fegetexceptflag.c \
  libc-top-half/musl/src/fenv/feholdexcept.c \
  libc-top-half/musl/src/exit/exit.c \
  libc-top-half/musl/src/exit/atexit.c \
  libc-top-half/musl/src/exit/assert.c \
  libc-top-half/musl/src/exit/quick_exit.c \
  libc-top-half/musl/src/exit/at_quick_exit.c \
  libc-top-half/musl/src/time/strftime.c \
  libc-top-half/musl/src/time/asctime.c \
  libc-top-half/musl/src/time/asctime_r.c \
  libc-top-half/musl/src/time/ctime.c \
  libc-top-half/musl/src/time/ctime_r.c \
  libc-top-half/musl/src/time/wcsftime.c \
  libc-top-half/musl/src/time/strptime.c \
  libc-top-half/musl/src/time/difftime.c \
  libc-top-half/musl/src/time/timegm.c \
  libc-top-half/musl/src/time/ftime.c \
  libc-top-half/musl/src/time/gmtime.c \
  libc-top-half/musl/src/time/gmtime_r.c \
  libc-top-half/musl/src/time/timespec_get.c \
  libc-top-half/musl/src/time/getdate.c \
  libc-top-half/musl/src/time/localtime.c \
  libc-top-half/musl/src/time/localtime_r.c \
  libc-top-half/musl/src/time/mktime.c \
  libc-top-half/musl/src/time/__tm_to_secs.c \
  libc-top-half/musl/src/time/__month_to_secs.c \
  libc-top-half/musl/src/time/__secs_to_tm.c \
  libc-top-half/musl/src/time/__year_to_secs.c \
  libc-top-half/musl/src/time/__tz.c \
  libc-top-half/musl/src/fcntl/creat.c \
  libc-top-half/musl/src/dirent/alphasort.c \
  libc-top-half/musl/src/dirent/versionsort.c \
  libc-top-half/musl/src/env/__stack_chk_fail.c \
  libc-top-half/musl/src/env/clearenv.c \
  libc-top-half/musl/src/env/getenv.c \
  libc-top-half/musl/src/env/putenv.c \
  libc-top-half/musl/src/env/setenv.c \
  libc-top-half/musl/src/env/unsetenv.c \
  libc-top-half/musl/src/unistd/posix_close.c \
  libc-top-half/musl/src/stat/futimesat.c \
  libc-top-half/musl/src/legacy/getpagesize.c \
  libc-top-half/musl/src/thread/thrd_sleep.c \
  \
  $(filter-out %/procfdname.c %/syscall.c %/syscall_ret.c %/vdso.c %/version.c, \
               libc-top-half/musl/src/internal/*.c) \
  $(filter-out %/flockfile.c %/funlockfile.c %/__lockfile.c %/ftrylockfile.c \
               %/rename.c \
               %/tmpnam.c %/tmpfile.c %/tempnam.c \
               %/popen.c %/pclose.c \
               %/remove.c \
               %/gets.c, \
               libc-top-half/musl/src/stdio/*.c) \
  $(filter-out %/strsignal.c, \
               libc-top-half/musl/src/string/*.c) \
  $(filter-out %/dcngettext.c %/textdomain.c %/bind_textdomain_codeset.c, \
               libc-top-half/musl/src/locale/*.c) \
  libc-top-half/musl/src/stdlib/*.c \
  libc-top-half/musl/src/search/*.c \
  libc-top-half/musl/src/multibyte/*.c \
  libc-top-half/musl/src/regex/*.c \
  libc-top-half/musl/src/prng/*.c \
  libc-top-half/musl/src/conf/*.c \
  libc-top-half/musl/src/ctype/*.c \
  $(filter-out %/__signbit.c %/__signbitf.c %/__signbitl.c \
               %/__fpclassify.c %/__fpclassifyf.c %/__fpclassifyl.c \
               %/ceilf.c %/ceil.c \
               %/floorf.c %/floor.c \
               %/truncf.c %/trunc.c \
               %/rintf.c %/rint.c \
               %/nearbyintf.c %/nearbyint.c \
               %/sqrtf.c %/sqrt.c \
               %/fabsf.c %/fabs.c \
               %/copysignf.c %/copysign.c \
               %/fminf.c %/fmaxf.c \
               %/fmin.c %/fmax.c, \
               libc-top-half/musl/src/math/*.c) \
  $(filter-out %/crealf.c %/creal.c %creall.c \
               %/cimagf.c %/cimag.c %cimagl.c, \
               libc-top-half/musl/src/complex/*.c) \
  libc-top-half/musl/src/crypt/*.c \
) # LIBC_TOP_HALF_MUSL_SOURCES

[ $THREAD_MODEL = posix ] && LIBC_TOP_HALF_MUSL_SOURCES+=(
  libc-top-half/musl/src/env/__init_tls.c \
  libc-top-half/musl/src/stdio/__lockfile.c \
  libc-top-half/musl/src/stdio/flockfile.c \
  libc-top-half/musl/src/stdio/ftrylockfile.c \
  libc-top-half/musl/src/stdio/funlockfile.c \
  libc-top-half/musl/src/thread/__lock.c \
  libc-top-half/musl/src/thread/__wait.c \
  libc-top-half/musl/src/thread/__timedwait.c \
  libc-top-half/musl/src/thread/default_attr.c \
  libc-top-half/musl/src/thread/pthread_attr_destroy.c \
  libc-top-half/musl/src/thread/pthread_attr_get.c \
  libc-top-half/musl/src/thread/pthread_attr_init.c \
  libc-top-half/musl/src/thread/pthread_attr_setstack.c \
  libc-top-half/musl/src/thread/pthread_attr_setdetachstate.c \
  libc-top-half/musl/src/thread/pthread_attr_setstacksize.c \
  libc-top-half/musl/src/thread/pthread_barrier_destroy.c \
  libc-top-half/musl/src/thread/pthread_barrier_init.c \
  libc-top-half/musl/src/thread/pthread_barrier_wait.c \
  libc-top-half/musl/src/thread/pthread_cleanup_push.c \
  libc-top-half/musl/src/thread/pthread_cond_broadcast.c \
  libc-top-half/musl/src/thread/pthread_cond_destroy.c \
  libc-top-half/musl/src/thread/pthread_cond_init.c \
  libc-top-half/musl/src/thread/pthread_cond_signal.c \
  libc-top-half/musl/src/thread/pthread_cond_timedwait.c \
  libc-top-half/musl/src/thread/pthread_cond_wait.c \
  libc-top-half/musl/src/thread/pthread_condattr_destroy.c \
  libc-top-half/musl/src/thread/pthread_condattr_init.c \
  libc-top-half/musl/src/thread/pthread_condattr_setclock.c \
  libc-top-half/musl/src/thread/pthread_condattr_setpshared.c \
  libc-top-half/musl/src/thread/pthread_create.c \
  libc-top-half/musl/src/thread/pthread_detach.c \
  libc-top-half/musl/src/thread/pthread_equal.c \
  libc-top-half/musl/src/thread/pthread_getspecific.c \
  libc-top-half/musl/src/thread/pthread_join.c \
  libc-top-half/musl/src/thread/pthread_key_create.c \
  libc-top-half/musl/src/thread/pthread_mutex_consistent.c \
  libc-top-half/musl/src/thread/pthread_mutex_destroy.c \
  libc-top-half/musl/src/thread/pthread_mutex_init.c \
  libc-top-half/musl/src/thread/pthread_mutex_getprioceiling.c \
  libc-top-half/musl/src/thread/pthread_mutex_lock.c \
  libc-top-half/musl/src/thread/pthread_mutex_timedlock.c \
  libc-top-half/musl/src/thread/pthread_mutex_trylock.c \
  libc-top-half/musl/src/thread/pthread_mutex_unlock.c \
  libc-top-half/musl/src/thread/pthread_mutexattr_destroy.c \
  libc-top-half/musl/src/thread/pthread_mutexattr_init.c \
  libc-top-half/musl/src/thread/pthread_mutexattr_setprotocol.c \
  libc-top-half/musl/src/thread/pthread_mutexattr_setpshared.c \
  libc-top-half/musl/src/thread/pthread_mutexattr_setrobust.c \
  libc-top-half/musl/src/thread/pthread_mutexattr_settype.c \
  libc-top-half/musl/src/thread/pthread_once.c \
  libc-top-half/musl/src/thread/pthread_rwlock_destroy.c \
  libc-top-half/musl/src/thread/pthread_rwlock_init.c \
  libc-top-half/musl/src/thread/pthread_rwlock_rdlock.c \
  libc-top-half/musl/src/thread/pthread_rwlock_timedrdlock.c \
  libc-top-half/musl/src/thread/pthread_rwlock_timedwrlock.c \
  libc-top-half/musl/src/thread/pthread_rwlock_tryrdlock.c \
  libc-top-half/musl/src/thread/pthread_rwlock_trywrlock.c \
  libc-top-half/musl/src/thread/pthread_rwlock_unlock.c \
  libc-top-half/musl/src/thread/pthread_rwlock_wrlock.c \
  libc-top-half/musl/src/thread/pthread_rwlockattr_destroy.c \
  libc-top-half/musl/src/thread/pthread_rwlockattr_init.c \
  libc-top-half/musl/src/thread/pthread_rwlockattr_setpshared.c \
  libc-top-half/musl/src/thread/pthread_setcancelstate.c \
  libc-top-half/musl/src/thread/pthread_setspecific.c \
  libc-top-half/musl/src/thread/pthread_self.c \
  libc-top-half/musl/src/thread/pthread_spin_destroy.c \
  libc-top-half/musl/src/thread/pthread_spin_init.c \
  libc-top-half/musl/src/thread/pthread_spin_lock.c \
  libc-top-half/musl/src/thread/pthread_spin_trylock.c \
  libc-top-half/musl/src/thread/pthread_spin_unlock.c \
  libc-top-half/musl/src/thread/pthread_testcancel.c \
  libc-top-half/musl/src/thread/sem_destroy.c \
  libc-top-half/musl/src/thread/sem_getvalue.c \
  libc-top-half/musl/src/thread/sem_init.c \
  libc-top-half/musl/src/thread/sem_post.c \
  libc-top-half/musl/src/thread/sem_timedwait.c \
  libc-top-half/musl/src/thread/sem_trywait.c \
  libc-top-half/musl/src/thread/sem_wait.c \
  libc-top-half/musl/src/thread/wasm32/wasi_thread_start.s \
) # LIBC_TOP_HALF_MUSL_SOURCES if [ $THREAD_MODEL = posix ]

EMMALLOC_SOURCES=( emmalloc/emmalloc.c )
LIBC_BOTTOM_HALF_ALL_SOURCES=(
  $(find libc-bottom-half/cloudlibc/src -name \*.c) \
  $(find libc-bottom-half/sources -name \*.c) \
)
LIBC_TOP_HALF_ALL_SOURCES=(
  ${LIBC_TOP_HALF_MUSL_SOURCES[@]} \
  $(find libc-top-half/sources -name \*.[cs]) \
)
LIBC_SOURCES=(
  ${EMMALLOC_SOURCES[@]} \
  ${LIBC_BOTTOM_HALF_ALL_SOURCES[@]} \
  ${LIBC_TOP_HALF_ALL_SOURCES[@]} \
)
LIBWASI_EMULATED_MMAN_SOURCES=( libc-bottom-half/mman/*.c )
LIBWASI_EMULATED_PROCESS_CLOCKS_SOURCES=( libc-bottom-half/clocks/*.c )
LIBWASI_EMULATED_GETPID_SOURCES=( libc-bottom-half/getpid/*.c )
LIBWASI_EMULATED_SIGNAL_SOURCES=(
  libc-bottom-half/signal/*.c \
  libc-top-half/musl/src/signal/psignal.c \
  libc-top-half/musl/src/string/strsignal.c \
)
CRT1_SOURCE=libc-bottom-half/crt/crt1.c
CRT1_COMMAND_SOURCE=libc-bottom-half/crt/crt1-command.c
CRT1_REACTOR_SOURCE=libc-bottom-half/crt/crt1-reactor.c
LIBC_BOTTOM_HALF_CRT_SOURCES=(
  $CRT1_SOURCE \
  $CRT1_COMMAND_SOURCE \
  $CRT1_REACTOR_SOURCE \
)

rm -rf "$SOURCE_DESTDIR"
mkdir -p "$SOURCE_DESTDIR"

_copy LICENSE*         "$SOURCE_DESTDIR"/
_copy emmalloc         "$SOURCE_DESTDIR"/emmalloc
_copy libc-bottom-half "$SOURCE_DESTDIR"/libc-bottom-half
_copy libc-top-half    "$SOURCE_DESTDIR"/libc-top-half

# remove stuff we don't need
rm -rf "$SOURCE_DESTDIR"/libc-bottom-half/headers/public
rm -rf "$SOURCE_DESTDIR"/libc-top-half/musl/include

for f in "$SOURCE_DESTDIR"/libc-top-half/musl/arch/*; do
  case "$f" in
    */generic|*/wasm32|*/wasm64) ;; # keep
    *) rm -rf "$f" & ;; # delete the rest
  esac
done
for f in "$SOURCE_DESTDIR"/libc-top-half/musl/*; do
  case "$f" in
    */arch|*/include|*/src|*/COPYRIGHT) ;; # keep
    *) rm -rf "$f" & ;; # delete the rest
  esac
done
wait

find "$SOURCE_DESTDIR" -type f \( \
  -name '*.in' -o -name '*.mak' -o -name 'README*' -o -name '.*' \
  -o -name '*.c' -o -name '*.s' -o -name '*.S' \
\) -delete

echo "copying sources..."
for f in \
  ${LIBC_SOURCES[@]} \
  ${LIBWASI_EMULATED_MMAN_SOURCES[@]} \
  ${LIBWASI_EMULATED_PROCESS_CLOCKS_SOURCES[@]} \
  ${LIBWASI_EMULATED_GETPID_SOURCES[@]} \
  ${LIBWASI_EMULATED_SIGNAL_SOURCES[@]} \
  ${LIBC_BOTTOM_HALF_CRT_SOURCES[@]} \
;do
  cp "$f" "$SOURCE_DESTDIR/$f"
done

# remove any .DS_Store files (and other hidden files)
find "$SOURCE_DESTDIR" -name '.*' -delete

# delete any remaining empty directories
find "$SOURCE_DESTDIR" -empty -type d -delete

# ————————————————————————————————————————————————————————————————————————————————————
# simplify source file tree by eliding "libc-bottom-half" and "libc-top-half" dirs

_pushd "$SOURCE_DESTDIR"

mv libc-bottom-half/headers/private headers-bottom
mv libc-top-half/headers/private    headers-top

for f in libc-top-half/*; do
  mv "$f" "${f:14}"
done
for f in $(find libc-bottom-half); do
  [[ "$f" != "."* ]] || continue
  [ "$f" != libc-bottom-half ] || continue
  if [ -d "$f" ]; then
    mkdir -p "${f:17}"
  else
    [ ! -e "${f:17}" ] || _err "duplicate file ${f:17}"
    mv "$f" "${f:17}"
  fi
done

find . -empty -type d -delete

# musl sources has relative "user-level" includes, eg '#include "../../include/errno.h"'
ln -s ../../sysinc/wasm32-wasi musl/include

# # ————————————————————————————————————————————————————————————————————————————————————
# # remove duplicate headers (headers that are in both sysinc and wasi)

# _pushd "$SOURCE_DESTDIR"

# echo "Deduplicating headers..."

# _remove_if_duplicate() { # <srcfile> <name>
#   local srcfile=$1
#   local name=$2
#   local incfile="$HEADER_DESTDIR/$name"
#   if [ -f "$incfile" ]; then
#     local shasum=$(sha256sum "$srcfile" | cut -d' ' -f1)
#     if [ "$(sha256sum "$incfile" | cut -d' ' -f1)" = "$shasum" ]; then
#       # echo "rm duplicate $(_relpath "$(realpath "$srcfile")")"
#       # echo "             $(_relpath "$incfile")"
#       rm $srcfile
#     fi
#   fi
# }

# for archdir in libc-top-half/musl/arch/*; do
#   for srcfile in $(find $archdir -type f -name '*.h'); do
#     _remove_if_duplicate "$srcfile" "${srcfile:$(( ${#archdir} + 1 ))}"
#   done
# done

# find . -empty -type d -delete

# ————————————————————————————————————————————————————————————————————————————————————

_pushd "$SOURCE_DESTDIR"
echo "generating $(_relpath "$INFO_H_FILE")"

_srcfile() {
  case "$1" in
    libc-bottom-half/*) echo ${1:17} ;;
    libc-top-half/*)    echo ${1:14} ;;
    *)                  echo $1 ;;
  esac
}

_srclist() {
  local f
  for f in "$@"; do
    case "$f" in
      libc-bottom-half/*) f=${f:17} ;;
      libc-top-half/*)    f=${f:14} ;;
    esac
    echo "  \"$f\","
  done
}

cat << END > "$INFO_H_FILE"
// Do not edit! Generated by ${SCRIPT_FILE##$PROJECT/}
// SPDX-License-Identifier: Apache-2.0

static const char* wasi_libc_top_sources[] = {
$(_srclist ${LIBC_TOP_HALF_ALL_SOURCES[@]})
};

static const char* wasi_libc_bottom_sources[] = {
$(_srclist ${LIBC_BOTTOM_HALF_ALL_SOURCES[@]})
};

static const char* wasi_emmalloc_sources[] = {
$(_srclist ${EMMALLOC_SOURCES[@]})
};

static const char* wasi_mman_sources[] = {
$(_srclist ${LIBWASI_EMULATED_MMAN_SOURCES[@]})
};

static const char* wasi_clocks_sources[] = {
$(_srclist ${LIBWASI_EMULATED_PROCESS_CLOCKS_SOURCES[@]})
};

static const char* wasi_getpid_sources[] = {
$(_srclist ${LIBWASI_EMULATED_GETPID_SOURCES[@]})
};

static const char* wasi_signal_sources[] = {
$(_srclist ${LIBWASI_EMULATED_SIGNAL_SOURCES[@]})
};

static const char* wasi_crt1_source = "$(_srcfile $CRT1_SOURCE)";
static const char* wasi_crt1_command_source = "$(_srcfile $CRT1_COMMAND_SOURCE)";
static const char* wasi_crt1_reactor_source = "$(_srcfile $CRT1_REACTOR_SOURCE)";
END

[ ! -f SRCLIST_ERROR ] || exit 1

# ————————————————————————————————————————————————————————————————————————————————————

_regenerate_sysinc_dir
