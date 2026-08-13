#!/usr/bin/env bash
# Integration check for the bronze host layer: boot bro-bronze-host headless,
# run a fixed number of frames, and compare the compiled app's output against a
# committed expectation, line for line.
#
# WHY THIS IS A SCRIPT AND NOT tests/<group>/test_*.js. bro's suite
# (tests/run_tests.sh) discovers `test_*.js` files and runs each one through
# bro-headless — the driver evaluates the file in the app's JS realm. There is
# no JS realm to evaluate anything in here: the subject IS the executable, an
# app compiled to machine code and linked into a different binary, and it has
# no scripting surface at all. So the file is named run_*.sh rather than
# test_*.js, which also keeps run_tests.sh from picking it up and handing it to
# a driver that would not know what to do with it.
#
# WHY IT IS NOT WIRED INTO run_tests.sh. bro-bronze-host does not exist unless
# somebody configured -DBRO_WITH_BRONZE=ON and supplied an app object; the
# default build has neither. Auto-discovering a check whose binary is absent
# from every normal build would make "missing" and "broken" the same result.
# Run it by hand (see README.md beside this file), or from CI once the bronze
# configuration is a build that runs there.
#
# Usage:
#   tests/bronze_host/run_bronze_host_test.sh
#
# Environment:
#   BRO_BRONZE_HOST   path to the bro-bronze-host executable (skips the search)
#   BRO_BRONZE_FRAMES frames to advance (default 8 — the app needs 5, and the
#                     spare ones must print nothing, which is itself a check:
#                     an app that keeps drawing after `done` has a rAF it never
#                     stopped rescheduling)

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/../.." && pwd)"
EXPECTED="$SCRIPT_DIR/expected/main_scenegraph.expected"
FRAMES="${BRO_BRONZE_FRAMES:-8}"

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
if [[ -n "${BRO_BRONZE_HOST:-}" ]]; then
    BIN="$BRO_BRONZE_HOST"
else
    for CANDIDATE in \
        "$PROJECT_DIR/build/Debug/bro-bronze-host.exe" \
        "$PROJECT_DIR/build/Release/bro-bronze-host.exe" \
        "$PROJECT_DIR/build/bro-bronze-host" \
        "$PROJECT_DIR/build-release/bro-bronze-host" \
        "$PROJECT_DIR/build-debug/bro-bronze-host"
    do
        [[ -f "$CANDIDATE" ]] && BIN="$CANDIDATE" && break
    done
fi

if [[ -z "$BIN" || ! -f "$BIN" ]]; then
    echo "  SKIP  bronze_host  (bro-bronze-host not built)"
    echo "        Build it: see src/bronze_host/README.md — it needs"
    echo "        -DBRO_WITH_BRONZE=ON and an app object."
    exit 77   # the automake convention for "skipped", not "passed"
fi

APP_DIR="$(to_win_path "$PROJECT_DIR/src/bronze_host/app/appdir")"

# stderr is folded in on purpose: the engine logs there, and a run that failed
# because a host global was missing says so in the log — swallowing it would
# leave a bare diff with no cause.
RAW="$("$BIN" "$APP_DIR" --headless --frames "$FRAMES" 2>&1)"
STATUS=$?

# `APP ` is the app's own prefix; everything else on the stream is engine log,
# which is neither deterministic nor this check's business. CR is stripped
# because host_main.cpp calls embed::runMain() rather than runProgram(), so
# bronze never puts stdout into binary mode and Windows expands each newline.
ACTUAL="$(printf '%s\n' "$RAW" | tr -d '\r' | grep '^APP ' || true)"

if [[ $STATUS -ne 0 ]]; then
    echo "  FAIL  bronze_host  (exit $STATUS)"
    printf '%s\n' "$RAW" | tail -20 | sed 's/^/        /'
    exit 1
fi

if diff -u "$EXPECTED" <(printf '%s\n' "$ACTUAL") > /tmp/bronze_host_diff.$$ 2>&1; then
    echo "  PASS  bronze_host"
    rm -f /tmp/bronze_host_diff.$$
    exit 0
fi

echo "  FAIL  bronze_host  (output differs from the pinned expectation)"
sed 's/^/        /' /tmp/bronze_host_diff.$$ | head -40
rm -f /tmp/bronze_host_diff.$$
exit 1
