#!/usr/bin/env bash
# Integration check for a wild three.js app with OrbitControls in a bronze-compiled app:
# boot bro-bronze-host-wild on appdir_wild under bro-headless's driver, and compare
# what was printed against a committed expectation.
#
# Usage:
#   tests/bronze_host/run_wild_test.sh
#
# Environment:
#   BRO_BRONZE_HOST_WILD  path to the bro-bronze-host-wild executable
#
# Building it (see src/bronze_host/README.md for the general sequence):
#   bronze build tests/bronze_host/apps/wild_orbit_probe.js -o <obj> --emit-obj \
#       --host-globals src/bronze_host/web_host.globals
#   cmake -B build -DBRO_WITH_BRONZE=ON -DBRO_BRONZE_APPS="wild=<obj>"
#   cmake --build build --config Release --target bro-bronze-host-wild

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/../.." && pwd)"
EXPECTED="$SCRIPT_DIR/expected/wild_orbit_probe.expected"

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
if [[ -n "${BRO_BRONZE_HOST_WILD:-}" ]]; then
    BIN="$BRO_BRONZE_HOST_WILD"
else
    for CANDIDATE in \
        "$PROJECT_DIR/build/Debug/bro-bronze-host-wild.exe" \
        "$PROJECT_DIR/build/Release/bro-bronze-host-wild.exe" \
        "$PROJECT_DIR/build/bro-bronze-host-wild" \
        "$PROJECT_DIR/build-release/bro-bronze-host-wild" \
        "$PROJECT_DIR/build-debug/bro-bronze-host-wild"
    do
        [[ -f "$CANDIDATE" ]] && BIN="$CANDIDATE" && break
    done
fi

if [[ -z "$BIN" || ! -f "$BIN" ]]; then
    echo "  SKIP  bronze_host_wild  (bro-bronze-host-wild not built)"
    echo "        Build it: see the header of this script — it needs"
    echo "        -DBRO_WITH_BRONZE=ON and -DBRO_BRONZE_APPS=\"wild=<obj>\"."
    exit 77   # the automake convention for "skipped", not "passed"
fi

APP_DIR="$(to_win_path "$SCRIPT_DIR/appdir_wild")"
DRIVER="$(to_win_path "$SCRIPT_DIR/drive_wild.js")"

rm -f wild_before.png wild_after.png

RAW="$("$BIN" "$APP_DIR" --headless "$DRIVER" 2>&1)"
STATUS=$?

CLEAN="$(printf '%s\n' "$RAW" | tr -d '\r')"
# Block 1: the compiled app, which prints bare lines on stdout.
APP_LINES="$(printf '%s\n' "$CLEAN" | grep '^APP ' || true)"
# Block 2: the driver, whose console.log lands in the engine log
JS_LINES="$(printf '%s\n' "$CLEAN" \
    | sed -n 's/^.*\[console\] \(\(PAGE\|DRV\) .*\)$/\1/p' || true)"
ACTUAL="$(printf '%s\n%s\n' "$APP_LINES" "$JS_LINES")"

if [[ $STATUS -ne 0 ]]; then
    echo "  FAIL  bronze_host_wild  (exit $STATUS)"
    printf '%s\n' "$CLEAN" | tail -20 | sed 's/^/        /'
    exit 1
fi

if diff -u "$EXPECTED" <(printf '%s\n' "$ACTUAL") > /tmp/bronze_wild_diff.$$ 2>&1; then
    echo "  PASS  bronze_host_wild"
    rm -f /tmp/bronze_wild_diff.$$
    exit 0
fi

echo "  FAIL  bronze_host_wild  (output differs from the pinned expectation)"
sed 's/^/        /' /tmp/bronze_wild_diff.$$ | head -60
rm -f /tmp/bronze_wild_diff.$$
exit 1
