#!/usr/bin/env bash
# Integration check for the FILE surface of the bronze host layer
# (src/bronze_host/host_file.cpp): boot bro-headless on appdir_file, let the
# compiled probe build Blobs, mint an object URL, read it back through fetch,
# and run a FileReader through every one of its outcomes.
#
# WHY THIS IS ITS OWN CHECK. Everything else in this layer moves data one way —
# the app names a path, the host reads it off disk. This is the only surface
# where the app HOLDS the bytes and needs somewhere to put them that the rest of
# the platform accepts. Its central failure is quiet: a `blob:` URL that mints
# but does not resolve reports success at every step and then answers 404 for a
# resource that was never a file.
#
# WHY IT ADVANCES MORE FRAMES THAN ITS NEIGHBOURS. A FileReader read is
# asynchronous by specification, and the fetch chain settles across several
# microtask checkpoints and host-task drains. The probe prints `done` from the
# fourth frame; anything still in flight after that would show up as a missing
# line rather than as a hang.
#
# Everything else follows run_node_test.sh: exit 0 pass, 1 fail, 77 skip.
#
# Usage:
#   tests/bronze_host/run_file_test.sh
#
# Environment:
#   BRO_HEADLESS       path to bro-headless (default: found under build/)
#   BRONZE             path to the bronze CLI (default: found under build/)
#   BRO_BRONZE_FRAMES  frames to advance (default 8)

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/../.." && pwd)"
EXPECTED="$SCRIPT_DIR/expected/file_probe.expected"
FRAMES="${BRO_BRONZE_FRAMES:-8}"

source "$SCRIPT_DIR/lib.sh"

BIN="$(bh_find_bro_headless "$PROJECT_DIR")" || {
    echo "  SKIP  bronze_host_file  (bro-headless not built)"
    exit 77   # the automake convention for "skipped", not "passed"
}

MODULE="$(bh_ensure_module "$PROJECT_DIR" "$SCRIPT_DIR/appdir_file" \
                           "$SCRIPT_DIR/apps/file_probe.js")"
case $? in
    0)  ;;
    77) echo "  SKIP  bronze_host_file  (no bronze CLI in this tree)"
        echo "        Configure with -DBRONZE_WITH_LLVM=ON to build one."
        exit 77 ;;
    *)  echo "  FAIL  bronze_host_file  (file_probe.js did not compile)"
        exit 1 ;;
esac

APP_DIR="$(bh_to_win_path "$SCRIPT_DIR/appdir_file")"

RAW="$("$BIN" "$APP_DIR" -e "advanceTime($((FRAMES * 16)))" 2>&1)"
STATUS=$?
CLEAN="$(printf '%s\n' "$RAW" | tr -d '\r')"
ACTUAL="$(printf '%s\n' "$CLEAN" | grep '^APP ' || true)"

if [[ $STATUS -ne 0 ]]; then
    echo "  FAIL  bronze_host_file  (exit $STATUS)"
    printf '%s\n' "$CLEAN" | tail -20 | sed 's/^/        /'
    exit 1
fi

if diff -u "$EXPECTED" <(printf '%s\n' "$ACTUAL") > /tmp/bronze_file_diff.$$ 2>&1; then
    echo "  PASS  bronze_host_file"
    rm -f /tmp/bronze_file_diff.$$
    exit 0
fi

echo "  FAIL  bronze_host_file  (output differs from the pinned expectation)"
sed 's/^/        /' /tmp/bronze_file_diff.$$ | head -80
rm -f /tmp/bronze_file_diff.$$
exit 1
