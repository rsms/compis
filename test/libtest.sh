# this script is sourced by each individual test script
source "$PROJECT/etc/lib.sh"
TEST_DIR="$PROJECT/test"
COEXE="${COEXE:-"$OUT_DIR/opt/co"}"
COPATH="$(dirname "$COEXE")"
COROOT=$PROJECT/lib
VERBOSE=${VERBOSE:-false}
VERY_VERBOSE=${VERY_VERBOSE:-false}

# note: tool commands are available in PATH as cc, ar etc
command -v co >/dev/null ||
  export PATH="$COPATH:$PATH"

if command -v wasmtime >/dev/null; then
  wasi-run() { wasmtime "$@"; }
else
  wasi-run() { echo "wasi-run: no WASI vm available; skipping"; }
fi
