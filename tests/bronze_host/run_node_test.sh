#!/usr/bin/env bash
# Integration check for the NODE surface of the bronze host layer
# (src/bronze_host/host_node.cpp): boot bro-headless on appdir_node, let the
# compiled probe exercise text nodes, comments, fragments and cloneNode, and
# compare what it printed against a committed expectation.
#
# WHY A SEPARATE CHECK FROM run_dom_test.sh. That one's subject is the DOM an
# app BUILDS — createElement, appendChild, read it back — and every tree it
# looks at is made of elements the probe itself created. This one's subject is
# the three node kinds that are not elements, and the tree an app is HANDED.
# They fail differently: the element surface breaks loudly, by losing identity
# or returning a fake array, and the node surface breaks silently, by skipping
# nodes that are there. Two subjects, two expectations.
#
# WHY THE APPDIR CARRIES REAL MARKUP. appdir_node/index.html holds a paragraph
# with text between its elements and a comment at the end, written on one line
# so its child count follows from the markup rather than from its indentation.
# That element is the only thing here a compiled app cannot construct for
# itself, and it is where an elements-only childNodes and a correct one give
# different answers.
#
# Everything else follows run_dom_test.sh: exit 0 pass, 1 fail, 77 skip.
#
# Usage:
#   tests/bronze_host/run_node_test.sh
#
# Environment:
#   BRO_HEADLESS       path to bro-headless (default: found under build/)
#   BRONZE             path to the bronze CLI (default: found under build/)
#   BRO_BRONZE_FRAMES  frames to advance (default 4)

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/../.." && pwd)"
EXPECTED="$SCRIPT_DIR/expected/node_probe.expected"
FRAMES="${BRO_BRONZE_FRAMES:-4}"

source "$SCRIPT_DIR/lib.sh"

BIN="$(bh_find_bro_headless "$PROJECT_DIR")" || {
    echo "  SKIP  bronze_host_node  (bro-headless not built)"
    exit 77   # the automake convention for "skipped", not "passed"
}

MODULE="$(bh_ensure_module "$PROJECT_DIR" "$SCRIPT_DIR/appdir_node" \
                           "$SCRIPT_DIR/apps/node_probe.js")"
case $? in
    0)  ;;
    77) echo "  SKIP  bronze_host_node  (no bronze CLI in this tree)"
        echo "        Configure with -DBRONZE_WITH_LLVM=ON to build one."
        exit 77 ;;
    *)  echo "  FAIL  bronze_host_node  (node_probe.js did not compile)"
        exit 1 ;;
esac

APP_DIR="$(bh_to_win_path "$SCRIPT_DIR/appdir_node")"

RAW="$("$BIN" "$APP_DIR" -e "advanceTime($((FRAMES * 16)))" 2>&1)"
STATUS=$?
CLEAN="$(printf '%s\n' "$RAW" | tr -d '\r')"
ACTUAL="$(printf '%s\n' "$CLEAN" | grep '^APP ' || true)"

if [[ $STATUS -ne 0 ]]; then
    echo "  FAIL  bronze_host_node  (exit $STATUS)"
    printf '%s\n' "$CLEAN" | tail -20 | sed 's/^/        /'
    exit 1
fi

if diff -u "$EXPECTED" <(printf '%s\n' "$ACTUAL") > /tmp/bronze_node_diff.$$ 2>&1; then
    echo "  PASS  bronze_host_node"
    rm -f /tmp/bronze_node_diff.$$
    exit 0
fi

echo "  FAIL  bronze_host_node  (output differs from the pinned expectation)"
sed 's/^/        /' /tmp/bronze_node_diff.$$ | head -80
rm -f /tmp/bronze_node_diff.$$
exit 1
