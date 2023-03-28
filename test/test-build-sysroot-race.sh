#!/usr/bin/env bash
#
# Tests two or more processes racing to build sysroot
#
set -euo pipefail
source "$(dirname "$0")/../etc/lib.sh"
cd "$(dirname "$0")"

TEST_NAME=$(basename "$0" .sh)

# Max concurrent running processes. Note: test is skipped if PARALLELISM < 2
PARALLELISM=${PARALLELISM:-$(nproc)}

# Total processes to spawn
NTOTAL=${NTOTAL:-$(( $PARALLELISM * 2 ))}

COEXE=${COEXE:-}
WORK_DIR="${WORK_DIR:-"$OUT_DIR/$TEST_NAME"}"
COCACHE="${COCACHE:-"$WORK_DIR/COCACHE"}"
FINISHED_FILE="$WORK_DIR/finished-tests"

# unless coexe is provided in env, use default build
if [ -n "$COEXE" ]; then
  f="$COEXE"
  [[ "$COEXE" == "/"* ]] || f="$PWD0/$f"
  [ -e "$f" ] || _err "$COEXE: not found"
  [ -f "$f" ] || _err "$COEXE: not a file"
  [ -x "$f" ] || _err "$COEXE: not executable"
  COEXE="$(realpath "$f")"
else
  COEXE="$OUT_DIR/opt/co"
  echo  "$(_relpath "$PROJECT/build.sh") -no-lto"
  $BASH "$PROJECT/build.sh" -no-lto
fi

CO_DEVBUILD=false
if "$COEXE" version | grep -q ' (dev '; then
  CO_DEVBUILD=true
fi

_build_one() { # <id>
  local id=$1
  set +e
  # CO_CBUILD_DONE_MARK_FILE enables cbuild to write a marker file when done
  # Only used for dev builds
  CO_CBUILD_MARKFILE_DIR="$WORK_DIR" \
  "$COEXE" cc --target=wasm32-none -o "$WORK_DIR/$id.out" \
    -c "$PROJECT/examples/hello-wasm/hello.c"
  local status=$?
  set -e
  echo "_build_one $id exited ($status)"
  printf "." >> "$FINISHED_FILE"
  return $status
}

# track running jobs
if [ $PARALLELISM -lt 2 ]; then
  echo "$0: WARNING: test disabled since host has only one available CPU" >&2
  exit 0
fi

# must start with empty COCACHE (even the dir must not exist, to test mkdir race)
export COCACHE
rm -rf "$COCACHE" "$WORK_DIR"
mkdir -p "$WORK_DIR"
echo "COEXE=$(_relpath "$COEXE")"
echo "WORK_DIR=$(_relpath "$WORK_DIR")"
echo "COCACHE=$(_relpath "$COCACHE")"
if [ -t 1 ]; then
  echo "note: you may be seeing less than $(( $NTOTAL - 1 )) \"waiting...\" messages, which "
  echo "      may be over-written on stdout by \"fancy\" status lines."
fi

# bsd or gnu style stat?
printf "...." > "$WORK_DIR/stat-test"
STAT_SIZE_ARGS='-c %s' # GNU style
if [ "$(stat $STAT_SIZE_ARGS "$WORK_DIR/stat-test" 2>/dev/null)" != "4" ]; then
  STAT_SIZE_ARGS='-f %z' # BSD-style
fi

# warm up process to minimize latency in spawning
"$COEXE" version >/dev/null

# terminate all subprocesses if user interrupts (^C) this script
trap "kill -15 -$$; return 1" SIGINT

# spawn jobs in parallel
touch "$FINISHED_FILE"
nstarted=0
nfinished=0
for (( i = 1; i < $NTOTAL + 1; i++ )); do
  # make sure to have no more than PARALLELISM subprocesses running at once
  nfinished=$(stat $STAT_SIZE_ARGS "$FINISHED_FILE")
  while [ $(( $nstarted - $nfinished )) -ge $PARALLELISM ]; do
    sleep 0.1
    nfinished=$(stat $STAT_SIZE_ARGS "$FINISHED_FILE")
  done
  (( nstarted = nstarted + 1 ))
  _build_one $i &
done

# wait for all jobs to complete
wait

# verify that every process produced output
echo "all finished; verifying output..."
for (( i = 1; i < $NTOTAL + 1; i++ )); do
  [ -f "$WORK_DIR/$i.out" ] || _err "_build_one $i did not produce output"
done

# verify that only one process built librt
if $CO_DEVBUILD; then
  donefiles=( $(cd "$WORK_DIR" && ls *.done) )
  if [ ${#donefiles[@]} -eq 0 ] || [ ! -f "$WORK_DIR/${donefiles[0]}" ]; then
    echo "No .done files! Bug in cbuild?" >&2
    exit 1
  elif [ ${#donefiles[@]} -gt 1 ]; then
    echo "Multiple .done files! More than one process built librt" >&2
    for f in "${donefiles[@]}"; do echo "$WORK_DIR/$f"; done
    exit 1
  fi
fi

echo "$0: ok"

exit # XXX

# cleanup
rm -rf "$COCACHE" "$WORK_DIR"
exit 0
