#!/usr/bin/env bash
# Integration check for event dispatch into a bronze-compiled app: boot
# bro-bronze-host-events on the hybrid app dir under bro-headless's driver,
# and compare what the two worlds printed against a committed expectation.
#
# WHY A SECOND SCRIPT AND NOT A SECOND MODE OF THE FIRST. The subject is a
# different executable: run_bronze_host_test.sh pins `bro-bronze-host`, the
# scene-graph app, in the fixed-frame mode that needs no script. This one pins
# `bro-bronze-host-events`, the events probe, under a DRIVER script — which is
# the only mode that can produce a click. Same layer, two binaries, and the
# multi-app CMake surface (BRO_BRONZE_APPS) exists so both can be in one tree
# at once. Everything else here follows run_bronze_host_test.sh: exit 0 pass,
# 1 fail, 77 skip.
#
# WHY THE OUTPUT IS COMPARED AS TWO BLOCKS. The compiled app prints to STDOUT
# (bronze's console) and the interpreted halves — the appdir's page script and
# the driver script — print through the ENGINE LOG, which is stderr. Those are
# two OS streams with two buffers, so their INTERLEAVING is not something a
# byte-for-byte expectation may depend on. Each stream's own order is, and that
# is what is pinned: every `APP ` line in order, then every interpreted line in
# order. Causality across the boundary survives that split, because it is
# carried in the payload rather than in the interleaving — `PAGE fromApp=pong:one`
# can only exist if a compiled listener saw `page:toApp` and answered it.
#
# Usage:
#   tests/bronze_host/run_events_test.sh
#
# Environment:
#   BRO_BRONZE_HOST_EVENTS  path to the bro-bronze-host-events executable
#
# Building it (see src/bronze_host/README.md for the general sequence):
#   bronze build tests/bronze_host/apps/events_probe.js -o <obj> --emit-obj \
#       --host-globals src/bronze_host/threejs_host.globals
#   cmake -B build -DBRO_WITH_BRONZE=ON -DBRO_BRONZE_APPS="events=<obj>"
#   cmake --build build --config Release --target bro-bronze-host-events

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/../.." && pwd)"
EXPECTED="$SCRIPT_DIR/expected/events_probe.expected"

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
if [[ -n "${BRO_BRONZE_HOST_EVENTS:-}" ]]; then
    BIN="$BRO_BRONZE_HOST_EVENTS"
else
    for CANDIDATE in \
        "$PROJECT_DIR/build/Debug/bro-bronze-host-events.exe" \
        "$PROJECT_DIR/build/Release/bro-bronze-host-events.exe" \
        "$PROJECT_DIR/build/bro-bronze-host-events" \
        "$PROJECT_DIR/build-release/bro-bronze-host-events" \
        "$PROJECT_DIR/build-debug/bro-bronze-host-events"
    do
        [[ -f "$CANDIDATE" ]] && BIN="$CANDIDATE" && break
    done
fi

if [[ -z "$BIN" || ! -f "$BIN" ]]; then
    echo "  SKIP  bronze_host_events  (bro-bronze-host-events not built)"
    echo "        Build it: see the header of this script — it needs"
    echo "        -DBRO_WITH_BRONZE=ON and -DBRO_BRONZE_APPS=\"events=<obj>\"."
    exit 77   # the automake convention for "skipped", not "passed"
fi

APP_DIR="$(to_win_path "$SCRIPT_DIR/appdir_events")"
DRIVER="$(to_win_path "$SCRIPT_DIR/drive_events.js")"

# stderr is folded in on purpose and twice over: it carries the engine log a
# failure explains itself in, AND the console output of both interpreted
# halves.
RAW="$("$BIN" "$APP_DIR" --headless "$DRIVER" 2>&1)"
STATUS=$?

CLEAN="$(printf '%s\n' "$RAW" | tr -d '\r')"
# Block 1: the compiled app, which prints bare lines on stdout.
APP_LINES="$(printf '%s\n' "$CLEAN" | grep '^APP ' || true)"
# Block 2: the page script and the driver, whose console.log lands in the
# engine log as `[hh:mm:ss.mmm] [INFO] [console] <text>`. The timestamp is
# stripped, obviously — it is the one part of the line that differs every run.
JS_LINES="$(printf '%s\n' "$CLEAN" \
    | sed -n 's/^.*\[console\] \(\(PAGE\|DRV\) .*\)$/\1/p' || true)"
ACTUAL="$(printf '%s\n%s\n' "$APP_LINES" "$JS_LINES")"

if [[ $STATUS -ne 0 ]]; then
    echo "  FAIL  bronze_host_events  (exit $STATUS)"
    printf '%s\n' "$CLEAN" | tail -20 | sed 's/^/        /'
    exit 1
fi

if diff -u "$EXPECTED" <(printf '%s\n' "$ACTUAL") > /tmp/bronze_events_diff.$$ 2>&1; then
    echo "  PASS  bronze_host_events"
    rm -f /tmp/bronze_events_diff.$$
    exit 0
fi

echo "  FAIL  bronze_host_events  (output differs from the pinned expectation)"
sed 's/^/        /' /tmp/bronze_events_diff.$$ | head -60
rm -f /tmp/bronze_events_diff.$$
exit 1
