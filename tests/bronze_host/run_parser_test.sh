#!/usr/bin/env bash
# Integration check for DOMParser in the bronze host layer
# (src/bronze_host/host_parser.cpp), and for the two DOM-level changes it
# forced.
#
# WHAT ONLY THIS CHECK CATCHES. Parsing a string into a tree is the easy half.
# The half that can go wrong quietly is that the result is a SECOND DOCUMENT:
#
#   * `scope.*` — a query on one document must not answer from the other. A
#     parsed document whose getElementById fell through to the live page would
#     look completely healthy until the day two documents used the same id.
#
#   * `adopt.*` — appending a parsed node into the live tree has to transfer
#     ownership, or the live tree ends up holding a node the parser document
#     will destroy. That step now lives in Node::appendChild (dom/element.cpp)
#     rather than in the JS bindings, because a compiled program appends
#     without passing through them; these lines are what pin it there.
#
#   * `obs.*` — a MutationObserver in the live document is installed FIRST, and
#     one in a parsed document second. Both must report. The host used to
#     remember only that some document had been hooked, which made the second
#     one silent; nothing could reach that bug before a second document existed.
#
# What this check does NOT cover, and why: the node registry had the same
# one-document assumption (host_element.cpp's freed-node observer), and
# exercising it means wrapping a parsed node before any live one and then
# freeing a live node — a use-after-free whose symptom is undefined rather than
# a wrong line. It is fixed by inspection.
#
# WHY IT ADVANCES SEVERAL FRAMES. Observer records are delivered once per frame
# from the bronze frame seam, so nothing has been reported when the compiled
# top level returns. The probe reports from the fourth frame; a record that
# never arrived prints as `absent` rather than hanging.
#
# Everything else follows run_node_test.sh: exit 0 pass, 1 fail, 77 skip.
#
# Usage:
#   tests/bronze_host/run_parser_test.sh
#
# Environment:
#   BRO_HEADLESS       path to bro-headless (default: found under build/)
#   BRONZE             path to the bronze CLI (default: found under build/)
#   BRO_BRONZE_FRAMES  frames to advance (default 8)

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/../.." && pwd)"
EXPECTED="$SCRIPT_DIR/expected/parser_probe.expected"
FRAMES="${BRO_BRONZE_FRAMES:-8}"

source "$SCRIPT_DIR/lib.sh"

BIN="$(bh_find_bro_headless "$PROJECT_DIR")" || {
    echo "  SKIP  bronze_host_parser  (bro-headless not built)"
    exit 77   # the automake convention for "skipped", not "passed"
}

MODULE="$(bh_ensure_module "$PROJECT_DIR" "$SCRIPT_DIR/appdir_parser" \
                           "$SCRIPT_DIR/apps/parser_probe.js")"
case $? in
    0)  ;;
    77) echo "  SKIP  bronze_host_parser  (no bronze CLI in this tree)"
        echo "        Configure with -DBRONZE_WITH_LLVM=ON to build one."
        exit 77 ;;
    *)  echo "  FAIL  bronze_host_parser  (parser_probe.js did not compile)"
        exit 1 ;;
esac

APP_DIR="$(bh_to_win_path "$SCRIPT_DIR/appdir_parser")"

RAW="$("$BIN" "$APP_DIR" -e "advanceTime($((FRAMES * 16)))" 2>&1)"
STATUS=$?
CLEAN="$(printf '%s\n' "$RAW" | tr -d '\r')"
ACTUAL="$(printf '%s\n' "$CLEAN" | grep '^APP ' || true)"

if [[ $STATUS -ne 0 ]]; then
    echo "  FAIL  bronze_host_parser  (exit $STATUS)"
    printf '%s\n' "$CLEAN" | tail -20 | sed 's/^/        /'
    exit 1
fi

if diff -u "$EXPECTED" <(printf '%s\n' "$ACTUAL") > /tmp/bronze_parser_diff.$$ 2>&1; then
    echo "  PASS  bronze_host_parser"
    rm -f /tmp/bronze_parser_diff.$$
    exit 0
fi

echo "  FAIL  bronze_host_parser  (output differs from the pinned expectation)"
sed 's/^/        /' /tmp/bronze_parser_diff.$$ | head -80
rm -f /tmp/bronze_parser_diff.$$
exit 1
