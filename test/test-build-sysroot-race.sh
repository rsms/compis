#!/usr/bin/env bash
#
# Tests two or more processes racing to build sysroot
#
set -euo pipefail
source "$(dirname "$0")/../etc/lib.sh"
cd "$(dirname "$0")"

TEST_NAME=$(basename "$0" .sh)

COEXE=${COEXE:-}
COCACHE="${COCACHE:-"$OUT_DIR/$TEST_NAME-cache"}"
NUM_RACING_PROCS=${NUM_RACING_PROCS:-4}

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

_build_one() { # <id>
  # build for wasi since it has small syslibs
  set +e
  "$COEXE" cc --target=wasm32-wasi data/hello.c -o "$OUT_DIR/$TEST_NAME-$1.exe"
  local status=$?
  set -e
  echo "_build_one $i exited ($status)"
  return $status
}

# must start without COCACHE, even the dir must not exist as this tests mkdir race
export COCACHE
rm -rf "$COCACHE" "$OUT_DIR/$TEST_NAME"-*.exe
echo "using COCACHE $(_relpath "$COCACHE")"

# warm up process to minimize latency in spawning
"$COEXE" version >/dev/null

# spawn jobs in parallel
for (( i = 1; i < $NUM_RACING_PROCS + 1; i++ )); do
  _build_one $i &
done

# wait for all jobs to complete, then verify that they produced output
wait
echo "all finished; verifying output..."
for (( i = 1; i < $NUM_RACING_PROCS + 1; i++ )); do
  [ -f "$OUT_DIR/$TEST_NAME-$i.exe" ] || _err "_build_one $i did not produce output"
done
echo "ok"

# cleanup
rm -rf "$COCACHE" "$OUT_DIR/$TEST_NAME"-*.exe