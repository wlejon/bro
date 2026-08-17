#!/usr/bin/env bash
# Integration check for the ELEMENT surface of the bronze host layer
# (src/bronze_host/host_element.cpp): boot bro-headless on appdir_dom, let
# the compiled probe build and take apart a small tree, and compare what it
# printed against a committed expectation.
#
# WHY A SEPARATE CHECK FROM run_bronze_host_test.sh. That one's subject is the
# RENDER path ? a scene graph drawn frame after frame ? and it says nothing
# about whether `panel.children[1] === beta`. This one's subject is the DOM
# itself: identity, real arrays, tree edits, classList, style, geometry. They
# fail for entirely different reasons, so they are two binaries and two
# expectations rather than two halves of one.
#
# WHY THERE IS NO DRIVER SCRIPT. The probe needs no input: it prints everything
# from its top level, which runs after engine init (HeadlessHooks::afterEngine).
# The frames exist only so the loop has somewhere to go afterwards ? an app that
# is still printing on frame 4 has a rAF it never stopped, and the expectation
# would catch that as extra lines.
#
# WHY THE GEOMETRY LINES ARE SAFE TO PIN. appdir_dom/index.html gives #panel an
# explicit content box in pixels with no border or padding, at the origin. Every
# number the probe reads follows from that stylesheet, so the expectation is
# written from the CSS rather than recorded from a run
# (tests/bronze_host/README.md).
#
# Everything else here follows run_events_test.sh: exit 0 pass, 1 fail, 77 skip.
#
# Usage:
#   tests/bronze_host/run_dom_test.sh
#
# Environment:
#   BRO_HEADLESS       path to bro-headless (default: found under build/)
#   BRONZE             path to the bronze CLI (default: found under build/)
#   BRO_BRONZE_FRAMES  frames to advance (default 4)
#
# THE BINARY IS THE STOCK ONE. There is no bro-bronze-host-dom any more: the
# probe compiles to appdir_dom/app.dll and the same bro-headless every other
# test in tests/ uses loads it. lib.sh rebuilds that module when it is missing
# or older than either the probe or the compiler, so editing dom_probe.js and
# re-running this script is the whole loop.

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/../.." && pwd)"
EXPECTED="$SCRIPT_DIR/expected/dom_probe.expected"
FRAMES="${BRO_BRONZE_FRAMES:-4}"

source "$SCRIPT_DIR/lib.sh"

BIN="$(bh_find_bro_headless "$PROJECT_DIR")" || {
    echo "  SKIP  bronze_host_dom  (bro-headless not built)"
    exit 77   # the automake convention for "skipped", not "passed"
}

MODULE="$(bh_ensure_module "$PROJECT_DIR" "$SCRIPT_DIR/appdir_dom" \
                           "$SCRIPT_DIR/apps/dom_probe.js")"
case $? in
    0)  ;;
    77) echo "  SKIP  bronze_host_dom  (no bronze CLI in this tree)"
        echo "        Configure with -DBRONZE_WITH_LLVM=ON to build one."
        exit 77 ;;
    *)  echo "  FAIL  bronze_host_dom  (dom_probe.js did not compile)"
        exit 1 ;;
esac

APP_DIR="$(bh_to_win_path "$SCRIPT_DIR/appdir_dom")"

# advanceTime drives the frames the probe's rAF needs. The retired per-app host
# took --frames because it owned its own main loop; bro-headless is driven from
# JS instead, and 16 ms a frame is what its virtual clock advances by.
RAW="$("$BIN" "$APP_DIR" -e "advanceTime($((FRAMES * 16)))" 2>&1)"
STATUS=$?
CLEAN="$(printf '%s\n' "$RAW" | tr -d '\r')"
# The compiled app prints bare `APP ` lines on stdout; the engine log is on the
# same capture here and everything in it is noise for this check.
ACTUAL="$(printf '%s\n' "$CLEAN" | grep '^APP ' || true)"

if [[ $STATUS -ne 0 ]]; then
    echo "  FAIL  bronze_host_dom  (exit $STATUS)"
    printf '%s\n' "$CLEAN" | tail -20 | sed 's/^/        /'
    exit 1
fi

if diff -u "$EXPECTED" <(printf '%s\n' "$ACTUAL") > /tmp/bronze_dom_diff.$$ 2>&1; then
    echo "  PASS  bronze_host_dom"
    rm -f /tmp/bronze_dom_diff.$$
    exit 0
fi

echo "  FAIL  bronze_host_dom  (output differs from the pinned expectation)"
sed 's/^/        /' /tmp/bronze_dom_diff.$$ | head -60
rm -f /tmp/bronze_dom_diff.$$
exit 1
