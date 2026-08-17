#!/usr/bin/env bash
# Integration check for the ABORT surface of the bronze host layer
# (src/bronze_host/host_abort.cpp): boot bro-headless on appdir_abort and put a
# compiled probe through AbortController, AbortSignal and the fetch that has to
# obey one.
#
# WHY THIS IS ITS OWN CHECK. Every assertion here is a NEGATIVE one — a fetch
# that must not deliver, a listener that must not fire twice, a reason that must
# not be replaced by a later abort — and every one of them fails silently. A
# signal that does nothing at all looks exactly like a signal that works, right
# up to the frame where the app renders the resource the user cancelled.
#
# WHY IT ADVANCES SEVERAL FRAMES. A bronze fetch settles on the next host-task
# drain and its chain then runs across microtask checkpoints, and one of the
# signals here is a 30 ms deadline (AbortSignal.timeout) that has to come due on
# the frame clock. The probe prints `done` from the fifth frame; anything still
# in flight at that point shows up as a missing line rather than as a hang.
#
# Everything else follows run_node_test.sh: exit 0 pass, 1 fail, 77 skip.
#
# Usage:
#   tests/bronze_host/run_abort_test.sh
#
# Environment:
#   BRO_HEADLESS       path to bro-headless (default: found under build/)
#   BRONZE             path to the bronze CLI (default: found under build/)
#   BRO_BRONZE_FRAMES  frames to advance (default 8)

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/../.." && pwd)"
EXPECTED="$SCRIPT_DIR/expected/abort_probe.expected"
FRAMES="${BRO_BRONZE_FRAMES:-8}"

source "$SCRIPT_DIR/lib.sh"

BIN="$(bh_find_bro_headless "$PROJECT_DIR")" || {
    echo "  SKIP  bronze_host_abort  (bro-headless not built)"
    exit 77   # the automake convention for "skipped", not "passed"
}

MODULE="$(bh_ensure_module "$PROJECT_DIR" "$SCRIPT_DIR/appdir_abort" \
                           "$SCRIPT_DIR/apps/abort_probe.js")"
case $? in
    0)  ;;
    77) echo "  SKIP  bronze_host_abort  (no bronze CLI in this tree)"
        echo "        Configure with -DBRONZE_WITH_LLVM=ON to build one."
        exit 77 ;;
    *)  echo "  FAIL  bronze_host_abort  (abort_probe.js did not compile)"
        exit 1 ;;
esac

APP_DIR="$(bh_to_win_path "$SCRIPT_DIR/appdir_abort")"

RAW="$("$BIN" "$APP_DIR" -e "advanceTime($((FRAMES * 16)))" 2>&1)"
STATUS=$?
CLEAN="$(printf '%s\n' "$RAW" | tr -d '\r')"
ACTUAL="$(printf '%s\n' "$CLEAN" | grep '^APP ' || true)"

if [[ $STATUS -ne 0 ]]; then
    echo "  FAIL  bronze_host_abort  (exit $STATUS)"
    printf '%s\n' "$CLEAN" | tail -20 | sed 's/^/        /'
    exit 1
fi

if diff -u "$EXPECTED" <(printf '%s\n' "$ACTUAL") > /tmp/bronze_abort_diff.$$ 2>&1; then
    echo "  PASS  bronze_host_abort"
    rm -f /tmp/bronze_abort_diff.$$
    exit 0
fi

echo "  FAIL  bronze_host_abort  (output differs from the pinned expectation)"
sed 's/^/        /' /tmp/bronze_abort_diff.$$ | head -80
rm -f /tmp/bronze_abort_diff.$$
exit 1
