#!/usr/bin/env bash
# Integration check for the ELEMENT surface of the bronze host layer
# (src/bronze_host/host_element.cpp): boot bro-bronze-host-dom headless, let
# the compiled probe build and take apart a small tree, and compare what it
# printed against a committed expectation.
#
# WHY A SEPARATE CHECK FROM run_bronze_host_test.sh. That one's subject is the
# RENDER path — a scene graph drawn frame after frame — and it says nothing
# about whether `panel.children[1] === beta`. This one's subject is the DOM
# itself: identity, real arrays, tree edits, classList, style, geometry. They
# fail for entirely different reasons, so they are two binaries and two
# expectations rather than two halves of one.
#
# WHY THERE IS NO DRIVER SCRIPT. The probe needs no input: it prints everything
# from its top level, which runs after engine init (HeadlessHooks::afterEngine).
# The frames exist only so the loop has somewhere to go afterwards — an app that
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
#   BRO_BRONZE_HOST_DOM  path to the bro-bronze-host-dom executable
#   BRO_BRONZE_FRAMES    frames to advance (default 4)
#
# Building it (see src/bronze_host/README.md for the general sequence):
#   bronze build tests/bronze_host/apps/dom_probe.js -o <obj> --emit-obj \
#       --host-globals src/bronze_host/web_host.globals
#   cmake -B build -DBRO_WITH_BRONZE=ON -DBRO_BRONZE_APPS="dom=<obj>"
#   cmake --build build --config Release --target bro-bronze-host-dom

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/../.." && pwd)"
EXPECTED="$SCRIPT_DIR/expected/dom_probe.expected"
FRAMES="${BRO_BRONZE_FRAMES:-4}"

# The .exe takes Windows paths; git-bash hands out /d/... ones.
to_win_path() {
    local p="$1"
    if [[ "${BIN:-}" == *.exe ]]; then
        if [[ "$p" =~ ^/mnt/([a-zA-Z])/(.*) ]]; then echo "${BASH_REMATCH[1]}:/${BASH_REMATCH[2]}"
        elif [[ "$p" =~ ^/([a-zA-Z])/(.*) ]]; then echo "${BASH_REMATCH[1]}:/${BASH_REMATCH[2]}"
        else echo "$p"; fi
    else
        echo "$p"
    fi
}

BIN=""
if [[ -n "${BRO_BRONZE_HOST_DOM:-}" ]]; then
    BIN="$BRO_BRONZE_HOST_DOM"
else
    for CANDIDATE in \
        "$PROJECT_DIR/build/Debug/bro-bronze-host-dom.exe" \
        "$PROJECT_DIR/build/Release/bro-bronze-host-dom.exe" \
        "$PROJECT_DIR/build/bro-bronze-host-dom" \
        "$PROJECT_DIR/build-release/bro-bronze-host-dom" \
        "$PROJECT_DIR/build-debug/bro-bronze-host-dom"
    do
        [[ -f "$CANDIDATE" ]] && BIN="$CANDIDATE" && break
    done
fi

if [[ -z "$BIN" || ! -f "$BIN" ]]; then
    echo "  SKIP  bronze_host_dom  (bro-bronze-host-dom not built)"
    echo "        Build it: see the header of this script — it needs"
    echo "        -DBRO_WITH_BRONZE=ON and -DBRO_BRONZE_APPS=\"dom=<obj>\"."
    exit 77   # the automake convention for "skipped", not "passed"
fi

APP_DIR="$(to_win_path "$SCRIPT_DIR/appdir_dom")"

RAW="$("$BIN" "$APP_DIR" --headless --frames "$FRAMES" 2>&1)"
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
