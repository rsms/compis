#!/usr/bin/env bash
set -euo pipefail

# note: libtest.sh includes lib.sh, so we do a little dance here to avoid
# including lib.sh twice
PWD0=$PWD
cd "$(dirname "$0")"
PROJECT=`cd .. && pwd`
source libtest.sh

#———————————————————————————————————————————————————————————————————————————————————————
# command line

DEBUG=false
VERBOSE=false
PARALLELISM=
PATTERNS=()
COEXE=

while [[ $# -gt 0 ]]; do case "$1" in
  --debug)   DEBUG=true; shift ;;
  -j*)       PARALLELISM=${1:2}; shift ;;
  -1)        PARALLELISM=1; shift ;;
  --coexe=*) COEXE=${1:8}; shift ;;
  -v)        VERBOSE=true; shift ;;
  -h|-help|--help) cat << _END
Usage: $0 [options] [--] [<testname> ...]
Options:
  --debug         Test with a debug build of compis instead of an opt one
  -jN             Run at most N tests in parallel (defaults to $(nproc))
  -1, -j1         Run one test at a time
  --coexe=<file>  Test specific, existing compis executable
  -v              Verbose output (implies -j1)
  -h, --help      Show help on stdout and exit
<testname>
  Only run tests which name matches this glob-style pattern.
  "name" is the name of the test file or directory, without extesion.
_END
    exit ;;
  --) shift; PATTERNS+=( "$@" ); break ;;
  -*) _err "Unknown option: $1" ;;
  *)  PATTERNS+=( "$1" ); shift ;;
esac; done

$VERBOSE && PARALLELISM=1
[ -n "$PARALLELISM" -a "$PARALLELISM" != 0 ] || PARALLELISM=$(nproc)

#———————————————————————————————————————————————————————————————————————————————————————
# setup

if [ -n "$COEXE" ]; then
  f=$COEXE
  [[ "$COEXE" == "/"* ]] || f="$PWD0/$f"
  [ -e "$f" ] || _err "$COEXE: not found"
  [ -f "$f" ] || _err "$COEXE: not a file"
  [ -x "$f" ] || _err "$COEXE: not executable"
  COEXE="$(realpath "$f")"
elif $DEBUG; then
  COEXE="$OUT_DIR/debug/co"
  echo  $(_relpath "$PROJECT/build.sh") -debug
  $BASH "$PROJECT/build.sh" -debug
else
  COEXE="$OUT_DIR/opt/co"
  echo  $(_relpath "$PROJECT/build.sh") -no-lto
  $BASH "$PROJECT/build.sh" -no-lto
fi

# create directory to run tests inside, copying template data into it
WORK_DIR="$OUT_DIR/test"
rm -rf "$WORK_DIR"
mkdir -p "$WORK_DIR"
FINISHED_FILE="$WORK_DIR/finished"
touch "$FINISHED_FILE"

_create_tool_symlinks "$COEXE"

# sanity check
for name in stdout.log stderr.log fail.log; do
  [ ! -e "$PWD/data/$name" ] || _err "data/$name conflicts with generated files!"
done

#———————————————————————————————————————————————————————————————————————————————————————
# run test scripts

export PROJECT
export COEXE
export VERBOSE
export PATH="$(dirname "$COEXE"):$PATH"

TESTS_DIR=tests

_run_test() { # <src-script> <n> <ntotal>
  local script_src="$1"
  local script_path="$TEST_DIR/$script_src"
  local name="${script_src:$(( ${#TESTS_DIR} + 1 ))}"
  local logname="${name%.sh}"
  local rundir="$WORK_DIR/$logname"
  local scriptname="$(basename "$name")"
  local script="$rundir/$scriptname"
  local n=$2
  local ntotal=$3
  local status

  mkdir -p "$rundir"
  cp -R "$TEST_DIR"/data/* "$rundir/"
  cat << END > "$script"
#!/usr/bin/env bash
set -euo pipefail
source "\$PROJECT/test/libtest.sh"
END
  cat "$script_src" >> "$script"

  if $VERBOSE && [ $PARALLELISM -eq 1 ]; then
    echo "[$n/$ntotal] $logname in $(_relpath "$rundir")/"
    if ! (cd "$rundir" && exec "$BASH" "$scriptname"); then
      echo "$(_relpath "$script_path"): FAILED"
      return 1
    fi
    return 0
  fi

  local out="$rundir/stdout.log"
  local err="$rundir/stderr.log"

  if [ $PARALLELISM -gt 1 ]; then
    $VERBOSE && echo "[$n/$ntotal] $logname: starting"
  else
    printf "[$n/$ntotal] $logname ..."
  fi

  set +e
  ( cd "$rundir" && exec "$BASH" "$scriptname" > "$out" 2> "$err" )
  status=$?
  set -e

  if [ $status -eq 0 ]; then
    if [ $PARALLELISM -gt 1 ]; then
      printf "." >> "$FINISHED_FILE"
      nfinished=$(stat $STAT_SIZE_ARGS "$FINISHED_FILE")
      local nrem=$(( $ntotal - $nfinished ))
      echo "[$nfinished/$ntotal] $logname: ok"
      [ -s "$err" ] && cat "$err" >&2
    else
      echo " ok"
    fi
    return 0
  fi

  # FAILED
  if [ $PARALLELISM -gt 1 ]; then
    echo "$logname: FAILED" > "$rundir/fail.log"
    echo "$(_relpath "$script_path") exited with status $status" > "$rundir/fail.log"
    if [ -s "$out" ]; then
      echo "---------- $logname stdout: ----------" >> "$rundir/fail.log"
      cat "$out" >> "$rundir/fail.log"
    fi
    echo "---------- $logname stderr: ----------" >> "$rundir/fail.log"
    if [ -s "$err" ]; then
      cat "$err" >> "$rundir/fail.log"
    else
      echo "(empty)" >> "$rundir/fail.log"
    fi
    echo "---------- $logname end ----------" >> "$rundir/fail.log"
    cat "$rundir/fail.log" >&2
  else
    echo " FAILED"
    echo "$(_relpath "$script_path") exited with status $status"
    [ ! -s "$out" ] || cat "$out"
    [ ! -s "$err" ] || cat "$err" >&2
    exit $status
  fi
  # interrupt all jobs ("-$$" = process group, $$ = leader pid)
  kill -2 -$$
  return 1
}

# find test scripts
# sort without filename extension so that e.g. a.sh is ordered before a-b.sh
TEST_SCRIPTS=( $(find $TESTS_DIR -type f -name \*.sh) )
IFS=$'\n' TEST_SCRIPTS=( $(sort -n <<< "${TEST_SCRIPTS[*]/%.sh/}") ); unset IFS
TEST_SCRIPTS=( "${TEST_SCRIPTS[@]/%/.sh}" )

if [ ${#PATTERNS[@]} -gt 0 ]; then
  shopt -s extglob
  TEST_SCRIPTS2=( "${TEST_SCRIPTS[@]}" )
  TEST_SCRIPTS=()
  for f in "${TEST_SCRIPTS2[@]}"; do
    name="${f:$(( ${#TESTS_DIR} + 1 ))}"
    name="${name%.sh}"
    for pat in "${PATTERNS[@]}"; do
      [[ $name == @($pat) ]] && TEST_SCRIPTS+=( "$f" )
    done
  done
fi

ntotal=${#TEST_SCRIPTS[@]}
nstarted=0
[ $ntotal = 0 ] && _err "no tests matches glob pattern(s): ${PATTERNS[@]}"
[ $ntotal = 1 ] && PARALLELISM=1

if [ $PARALLELISM -eq 1 ]; then
  for f in "${TEST_SCRIPTS[@]}"; do
    (( nstarted = nstarted + 1 ))
    _run_test "$f" $nstarted $ntotal
  done
  exit 0
fi

printf "...." > "$WORK_DIR/stat-test"
STAT_SIZE_ARGS='-c %s' # GNU style
if [ "$(stat $STAT_SIZE_ARGS "$WORK_DIR/stat-test" 2>/dev/null)" != "4" ]; then
  STAT_SIZE_ARGS='-f %z' # BSD-style
fi

nfinished=0

for f in "${TEST_SCRIPTS[@]}"; do
  # if the number of running jobs >= PARALLELISM, wait for at least one to finish
  nfinished=$(stat $STAT_SIZE_ARGS "$FINISHED_FILE")
  while [ $(( $nstarted - $nfinished )) -ge $PARALLELISM ]; do
    sleep 0.1
    nfinished=$(stat $STAT_SIZE_ARGS "$FINISHED_FILE")
  done
  (( nstarted = nstarted + 1 ))
  _run_test "$f" $nstarted $ntotal &
done

wait
