#!/usr/bin/env bash
# Integration check for the PHYSICS surface of the bronze host layer
# (src/bronze_host/host_physics.cpp): boot bro-headless on appdir_physics, let the
# compiled probe create Physics bodies, transforms, character controller, soft bodies,
# raycasts, overlaps, and test destruction.
#
# Usage:
#   tests/bronze_host/run_physics_test.sh
#
# Environment:
#   BRO_HEADLESS       path to bro-headless (default: found under build/)
#   BRONZE             path to the bronze CLI (default: found under build/)
#   BRO_BRONZE_FRAMES  frames to advance (default 8)

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/../.." && pwd)"
EXPECTED="$SCRIPT_DIR/expected/physics_probe.expected"
FRAMES="${BRO_BRONZE_FRAMES:-8}"

source "$SCRIPT_DIR/lib.sh"

BIN="$(bh_find_bro_headless "$PROJECT_DIR")" || {
    echo "  SKIP  bronze_host_physics  (bro-headless not built)"
    exit 77   # the automake convention for "skipped", not "passed"
}

MODULE="$(bh_ensure_module "$PROJECT_DIR" "$SCRIPT_DIR/appdir_physics" \
                           "$SCRIPT_DIR/apps/physics_probe.js")"
case $? in
    0)  ;;
    77) echo "  SKIP  bronze_host_physics  (no bronze CLI in this tree)"
        echo "        Configure with -DBRONZE_WITH_LLVM=ON to build one."
        exit 77 ;;
    *)  echo "  FAIL  bronze_host_physics  (physics_probe.js did not compile)"
        exit 1 ;;
esac

APP_DIR="$(bh_to_win_path "$SCRIPT_DIR/appdir_physics")"

RAW="$("$BIN" "$APP_DIR" -e "advanceTime($((FRAMES * 16)))" 2>&1)"
STATUS=$?
CLEAN="$(printf '%s\n' "$RAW" | tr -d '\r')"
ACTUAL="$(printf '%s\n' "$CLEAN" | grep '^APP ' || true)"

if [[ $STATUS -ne 0 ]]; then
    echo "  FAIL  bronze_host_physics  (exit $STATUS)"
    printf '%s\n' "$CLEAN" | tail -20 | sed 's/^/        /'
    exit 1
fi

EXP_CLEAN="$(tr -d '\r' < "$EXPECTED")"

if [[ "$ACTUAL" == "$EXP_CLEAN" ]]; then
    echo "  PASS  bronze_host_physics"
    exit 0
else
    echo "  FAIL  bronze_host_physics  (output mismatch)"
    diff -u --strip-trailing-cr "$EXPECTED" <(printf '%s\n' "$ACTUAL") | head -30 | sed 's/^/        /' || true
    exit 1
fi
