# this script is sourced by each individual test script
source "$PROJECT/etc/lib.sh"
TEST_DIR="$PROJECT/test"
COEXE="${COEXE:-"$OUT_DIR/opt/co"}"
VERBOSE=${VERBOSE:-false}

command -v co >/dev/null ||
  export PATH="$(dirname "$COEXE"):$PATH"

# note: tool commands are available in PATH as cc, ar etc
