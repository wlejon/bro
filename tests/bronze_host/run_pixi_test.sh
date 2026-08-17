#!/usr/bin/env bash
# Integration check for a Pixi.js sprite app in a bronze-compiled app:
# boot bro-headless on appdir_pixi under bro-headless's driver, and compare
# what was printed against a committed expectation.
#
# Usage:
#   tests/bronze_host/run_pixi_test.sh
#
# Environment:
#   BRO_HEADLESS          path to bro-headless (default: found under build/)
#   BRONZE                path to the bronze CLI (default: found under build/)
#
# Building it (see src/bronze_host/README.md for the general sequence):
#   bronze build tests/bronze_host/apps/pixi_sprites_probe.js -o <obj> --emit-obj \
#       --host-globals src/bronze_host/web_host.globals
#   cmake -B build -DBRO_WITH_BRONZE=ON -DBRO_BRONZE_APPS="pixi=<obj>"
#   cmake --build build --config Release --target bro-headless

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/../.." && pwd)"
EXPECTED="$SCRIPT_DIR/expected/pixi_sprites_probe.expected"

source "$SCRIPT_DIR/lib.sh"

BIN="$(bh_find_bro_headless "$PROJECT_DIR")" || {
    echo "  SKIP  bronze_host_pixi  (bro-headless not built)"
    exit 77   # the automake convention for "skipped", not "passed"
}

MODULE="$(bh_ensure_module "$PROJECT_DIR" "$SCRIPT_DIR/appdir_pixi"                            "$SCRIPT_DIR/apps/pixi_sprites_probe.js")"
case $? in
    0)  ;;
    77) echo "  SKIP  bronze_host_pixi  (no bronze CLI in this tree)"
        echo "        Configure with -DBRONZE_WITH_LLVM=ON to build one."
        exit 77 ;;
    *)  echo "  FAIL  bronze_host_pixi  (pixi_sprites_probe.js did not compile)"
        exit 1 ;;
esac

APP_DIR="$(bh_to_win_path "$SCRIPT_DIR/appdir_pixi")"
DRIVER="$(bh_to_win_path "$SCRIPT_DIR/drive_pixi.js")"

rm -f pixi_before.png pixi_after.png

RAW="$("$BIN" "$APP_DIR" "$DRIVER" 2>&1)"
STATUS=$?

CLEAN="$(printf '%s\n' "$RAW" | tr -d '\r')"
# Block 1: the compiled app, which prints bare lines on stdout.
APP_LINES="$(printf '%s\n' "$CLEAN" | grep '^APP ' || true)"
# Block 2: the driver, whose console.log lands in the engine log
JS_LINES="$(printf '%s\n' "$CLEAN" \
    | sed -n 's/^.*\[console\] \(\(PAGE\|DRV\) .*\)$/\1/p' || true)"
ACTUAL="$(printf '%s\n%s\n' "$APP_LINES" "$JS_LINES")"

if [[ $STATUS -ne 0 ]]; then
    echo "  FAIL  bronze_host_pixi  (exit $STATUS)"
    printf '%s\n' "$CLEAN" | tail -20 | sed 's/^/        /'
    exit 1
fi

if diff -u "$EXPECTED" <(printf '%s\n' "$ACTUAL") > /tmp/bronze_pixi_diff.$$ 2>&1; then
    echo "  PASS  bronze_host_pixi"
    rm -f /tmp/bronze_pixi_diff.$$
    exit 0
fi

echo "  FAIL  bronze_host_pixi  (output differs from the pinned expectation)"
sed 's/^/        /' /tmp/bronze_pixi_diff.$$ | head -60
rm -f /tmp/bronze_pixi_diff.$$
exit 1
