#!/usr/bin/env bash
# Integration check for the INPUT surface of the bronze host layer
# (pointerLock, fullscreen, and Gamepad API): boot bro-headless on appdir_input,
# connect a virtual gamepad, and compare probe output against pinned expectation.

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/../.." && pwd)"
EXPECTED="$SCRIPT_DIR/expected/input_probe.expected"

source "$SCRIPT_DIR/lib.sh"

BIN="$(bh_find_bro_headless "$PROJECT_DIR")" || {
    echo "  SKIP  bronze_host_input  (bro-headless not built)"
    exit 77
}

MODULE="$(bh_ensure_module "$PROJECT_DIR" "$SCRIPT_DIR/appdir_input" \
                           "$SCRIPT_DIR/apps/input_probe.js")"
case $? in
    0)  ;;
    77) echo "  SKIP  bronze_host_input  (no bronze CLI in this tree)"
        exit 77 ;;
    *)  echo "  FAIL  bronze_host_input  (input_probe.js did not compile)"
        exit 1 ;;
esac

APP_DIR="$(bh_to_win_path "$SCRIPT_DIR/appdir_input")"

RAW="$("$BIN" "$APP_DIR" -e "gamepadConnect('Virtual Controller'); gamepadButton(0, 'south', true, 1.0); gamepadAxis(0, 0, 0.75); advanceTime(64);" 2>&1)"
STATUS=$?
CLEAN="$(printf '%s\n' "$RAW" | tr -d '\r')"
ACTUAL="$(printf '%s\n' "$CLEAN" | grep '^APP ' || true)"

if [[ $STATUS -ne 0 ]]; then
    echo "  FAIL  bronze_host_input  (exit $STATUS)"
    printf '%s\n' "$CLEAN" | tail -20 | sed 's/^/        /'
    exit 1
fi

if diff -u "$EXPECTED" <(printf '%s\n' "$ACTUAL") > /tmp/bronze_input_diff.$$ 2>&1; then
    echo "  PASS  bronze_host_input"
    rm -f /tmp/bronze_input_diff.$$
    exit 0
fi

echo "  FAIL  bronze_host_input  (output differs from the pinned expectation)"
sed 's/^/        /' /tmp/bronze_input_diff.$$ | head -60
rm -f /tmp/bronze_input_diff.$$
exit 1
