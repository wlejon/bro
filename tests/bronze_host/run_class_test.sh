#!/usr/bin/env bash
# Integration check for HOST CLASSES in the bronze host layer, using `Image`
# as the worked example: a handle born on its constructor's prototype, methods
# shared once per class rather than closed over per instance, and `instanceof`
# answering. Boots bro-headless on appdir_class and compares probe output
# against the pinned expectation. The probe drives itself, so no injected
# input is needed.

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/../.." && pwd)"
EXPECTED="$SCRIPT_DIR/expected/class_probe.expected"

source "$SCRIPT_DIR/lib.sh"

BIN="$(bh_find_bro_headless "$PROJECT_DIR")" || {
    echo "  SKIP  bronze_host_class  (bro-headless not built)"
    exit 77
}

MODULE="$(bh_ensure_module "$PROJECT_DIR" "$SCRIPT_DIR/appdir_class" \
                           "$SCRIPT_DIR/apps/class_probe.js")"
case $? in
    0)  ;;
    77) echo "  SKIP  bronze_host_class  (no bronze CLI in this tree)"
        exit 77 ;;
    *)  echo "  FAIL  bronze_host_class  (class_probe.js did not compile)"
        exit 1 ;;
esac

APP_DIR="$(bh_to_win_path "$SCRIPT_DIR/appdir_class")"

RAW="$("$BIN" "$APP_DIR" -e "advanceTime(64);" 2>&1)"
STATUS=$?
CLEAN="$(printf '%s\n' "$RAW" | tr -d '\r')"
ACTUAL="$(printf '%s\n' "$CLEAN" | grep '^APP ' || true)"

if [[ $STATUS -ne 0 ]]; then
    echo "  FAIL  bronze_host_class  (exit $STATUS)"
    printf '%s\n' "$CLEAN" | tail -20 | sed 's/^/        /'
    exit 1
fi

if diff -u "$EXPECTED" <(printf '%s\n' "$ACTUAL") > /tmp/bronze_class_diff.$$ 2>&1; then
    echo "  PASS  bronze_host_class"
    rm -f /tmp/bronze_class_diff.$$
    exit 0
fi

echo "  FAIL  bronze_host_class  (output differs from the pinned expectation)"
sed 's/^/        /' /tmp/bronze_class_diff.$$ | head -60
rm -f /tmp/bronze_class_diff.$$
exit 1
