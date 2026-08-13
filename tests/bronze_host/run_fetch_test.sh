#!/usr/bin/env bash
# Integration check for fetch() in a bronze-compiled app: boot
# bro-bronze-host-fetch on the app dir under bro-headless's driver,
# and compare what was printed against a committed expectation.
#
# Usage:
#   tests/bronze_host/run_fetch_test.sh
#
# Environment:
#   BRO_BRONZE_HOST_FETCH  path to the bro-bronze-host-fetch executable
#
# Building it (see src/bronze_host/README.md for the general sequence):
#   bronze build tests/bronze_host/apps/fetch_probe.js -o <obj> --emit-obj \
#       --host-globals src/bronze_host/threejs_host.globals
#   cmake -B build -DBRO_WITH_BRONZE=ON -DBRO_BRONZE_APPS="fetch=<obj>"
#   cmake --build build --config Release --target bro-bronze-host-fetch

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/../.." && pwd)"
EXPECTED="$SCRIPT_DIR/expected/fetch_probe.expected"

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
if [[ -n "${BRO_BRONZE_HOST_FETCH:-}" ]]; then
    BIN="$BRO_BRONZE_HOST_FETCH"
else
    for CANDIDATE in \
        "$PROJECT_DIR/build/Debug/bro-bronze-host-fetch.exe" \
        "$PROJECT_DIR/build/Release/bro-bronze-host-fetch.exe" \
        "$PROJECT_DIR/build/bro-bronze-host-fetch" \
        "$PROJECT_DIR/build-release/bro-bronze-host-fetch" \
        "$PROJECT_DIR/build-debug/bro-bronze-host-fetch"
    do
        [[ -f "$CANDIDATE" ]] && BIN="$CANDIDATE" && break
    done
fi

if [[ -z "$BIN" || ! -f "$BIN" ]]; then
    echo "  SKIP  bronze_host_fetch  (bro-bronze-host-fetch not built)"
    echo "        Build it: see the header of this script — it needs"
    echo "        -DBRO_WITH_BRONZE=ON and -DBRO_BRONZE_APPS=\"fetch=<obj>\"."
    exit 77   # the automake convention for "skipped", not "passed"
fi

APP_DIR="$(to_win_path "$SCRIPT_DIR/appdir_fetch")"
DRIVER="$(to_win_path "$SCRIPT_DIR/drive_fetch.js")"

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
    echo "  FAIL  bronze_host_fetch  (exit $STATUS)"
    printf '%s\n' "$CLEAN" | tail -20 | sed 's/^/        /'
    exit 1
fi

if diff -u "$EXPECTED" <(printf '%s\n' "$ACTUAL") > /tmp/bronze_fetch_diff.$$ 2>&1; then
    echo "  PASS  bronze_host_fetch"
    rm -f /tmp/bronze_fetch_diff.$$
    exit 0
fi

echo "  FAIL  bronze_host_fetch  (output differs from the pinned expectation)"
sed 's/^/        /' /tmp/bronze_fetch_diff.$$ | head -60
rm -f /tmp/bronze_fetch_diff.$$
exit 1
