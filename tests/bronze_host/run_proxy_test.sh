#!/usr/bin/env bash
# Integration check for the four PROXY-backed live views of the bronze host
# layer (el.style, getComputedStyle, el.dataset, localStorage): boot
# bro-headless on appdir_proxy and compare probe output against the pinned
# expectation. The probe drives itself, so no injected input is needed.

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/../.." && pwd)"
EXPECTED="$SCRIPT_DIR/expected/proxy_probe.expected"

source "$SCRIPT_DIR/lib.sh"

BIN="$(bh_find_bro_headless "$PROJECT_DIR")" || {
    echo "  SKIP  bronze_host_proxy  (bro-headless not built)"
    exit 77
}

MODULE="$(bh_ensure_module "$PROJECT_DIR" "$SCRIPT_DIR/appdir_proxy" \
                           "$SCRIPT_DIR/apps/proxy_probe.js")"
case $? in
    0)  ;;
    77) echo "  SKIP  bronze_host_proxy  (no bronze CLI in this tree)"
        exit 77 ;;
    *)  echo "  FAIL  bronze_host_proxy  (proxy_probe.js did not compile)"
        exit 1 ;;
esac

APP_DIR="$(bh_to_win_path "$SCRIPT_DIR/appdir_proxy")"

RAW="$("$BIN" "$APP_DIR" -e "advanceTime(64);" 2>&1)"
STATUS=$?
CLEAN="$(printf '%s\n' "$RAW" | tr -d '\r')"
ACTUAL="$(printf '%s\n' "$CLEAN" | grep '^APP ' || true)"

if [[ $STATUS -ne 0 ]]; then
    echo "  FAIL  bronze_host_proxy  (exit $STATUS)"
    printf '%s\n' "$CLEAN" | tail -20 | sed 's/^/        /'
    exit 1
fi

if diff -u "$EXPECTED" <(printf '%s\n' "$ACTUAL") > /tmp/bronze_proxy_diff.$$ 2>&1; then
    echo "  PASS  bronze_host_proxy"
    rm -f /tmp/bronze_proxy_diff.$$
    exit 0
fi

echo "  FAIL  bronze_host_proxy  (output differs from the pinned expectation)"
sed 's/^/        /' /tmp/bronze_proxy_diff.$$ | head -60
rm -f /tmp/bronze_proxy_diff.$$
exit 1
