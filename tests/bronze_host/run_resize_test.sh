#!/usr/bin/env bash
# Integration check for ResizeObserver in the bronze host layer
# (src/bronze_host/host_observers.cpp).
#
# WHY THIS IS ITS OWN CHECK. ResizeObserver shares a file and a frame slot with
# MutationObserver and nothing else: a mutation is something the tree reports,
# and a size is something nobody reports, so this half is a poll of the layout
# box. Its two failures are both quiet. One is delivering nothing on the first
# pass ? the initial report is the reason code uses a ResizeObserver at all, and
# an implementation that only fires on CHANGE looks perfectly correct until the
# first layout. The other is delivering every frame, which is indistinguishable
# from working and runs an app's relayout callback sixty times a second forever.
#
# WHY IT ADVANCES SEVERAL FRAMES. The poll runs once per frame from the bronze
# frame seam, and the probe needs six of them: one for the initial report, one
# to resize inside a frame callback and be measured by the same frame's poll,
# one to prove an unchanged size reports nothing, and the rest to prove it keeps
# reporting nothing.
#
# Everything else follows run_node_test.sh: exit 0 pass, 1 fail, 77 skip.
#
# Usage:
#   tests/bronze_host/run_resize_test.sh
#
# Environment:
#   BRO_HEADLESS       path to bro-headless (default: found under build/)
#   BRONZE             path to the bronze CLI (default: found under build/)
#   BRO_BRONZE_FRAMES  frames to advance (default 8)

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/../.." && pwd)"
EXPECTED="$SCRIPT_DIR/expected/resize_probe.expected"
FRAMES="${BRO_BRONZE_FRAMES:-8}"

source "$SCRIPT_DIR/lib.sh"

BIN="$(bh_find_bro_headless "$PROJECT_DIR")" || {
    echo "  SKIP  bronze_host_resize  (bro-headless not built)"
    exit 77   # the automake convention for "skipped", not "passed"
}

MODULE="$(bh_ensure_module "$PROJECT_DIR" "$SCRIPT_DIR/appdir_resize" \
                           "$SCRIPT_DIR/apps/resize_probe.js")"
case $? in
    0)  ;;
    77) echo "  SKIP  bronze_host_resize  (no bronze CLI in this tree)"
        echo "        Configure with -DBRONZE_WITH_LLVM=ON to build one."
        exit 77 ;;
    *)  echo "  FAIL  bronze_host_resize  (resize_probe.js did not compile)"
        exit 1 ;;
esac

APP_DIR="$(bh_to_win_path "$SCRIPT_DIR/appdir_resize")"

RAW="$("$BIN" "$APP_DIR" -e "advanceTime($((FRAMES * 16)))" 2>&1)"
STATUS=$?
CLEAN="$(printf '%s\n' "$RAW" | tr -d '\r')"
ACTUAL="$(printf '%s\n' "$CLEAN" | grep '^APP ' || true)"

if [[ $STATUS -ne 0 ]]; then
    echo "  FAIL  bronze_host_resize  (exit $STATUS)"
    printf '%s\n' "$CLEAN" | tail -20 | sed 's/^/        /'
    exit 1
fi

if diff -u "$EXPECTED" <(printf '%s\n' "$ACTUAL") > /tmp/bronze_resize_diff.$$ 2>&1; then
    echo "  PASS  bronze_host_resize"
    rm -f /tmp/bronze_resize_diff.$$
    exit 0
fi

echo "  FAIL  bronze_host_resize  (output differs from the pinned expectation)"
sed 's/^/        /' /tmp/bronze_resize_diff.$$ | head -80
rm -f /tmp/bronze_resize_diff.$$
exit 1
