#!/usr/bin/env bash
# Integration check for MutationObserver in the bronze host layer
# (src/bronze_host/host_observers.cpp) and for the DOM-level notice it is built
# on (Document::notifyMutation, src/dom/document.h).
#
# WHY THIS IS ITS OWN CHECK, and what only it can catch. This layer could have
# implemented MutationObserver by watching its OWN mutators — everything a
# compiled app does to the tree goes through host_element.cpp and host_node.cpp
# — and every assertion here but one would still pass. The one is `page.*`: a
# script in the page's QuickJS realm sets an attribute, and the compiled
# observer has to hear about it. It only does because the notice is fired by
# dom::Element itself, which is the whole reason that hook was added rather
# than a second observer system next to the JS bindings' own.
#
# WHY IT ADVANCES SEVERAL FRAMES. Records are delivered once per frame from the
# bronze frame seam, so nothing at all has been reported when the compiled top
# level returns. The probe reports from the fifth frame; a record that never
# arrived prints as `absent` rather than hanging.
#
# Everything else follows run_node_test.sh: exit 0 pass, 1 fail, 77 skip.
#
# Usage:
#   tests/bronze_host/run_observer_test.sh
#
# Environment:
#   BRO_HEADLESS       path to bro-headless (default: found under build/)
#   BRONZE             path to the bronze CLI (default: found under build/)
#   BRO_BRONZE_FRAMES  frames to advance (default 8)

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/../.." && pwd)"
EXPECTED="$SCRIPT_DIR/expected/observer_probe.expected"
FRAMES="${BRO_BRONZE_FRAMES:-8}"

source "$SCRIPT_DIR/lib.sh"

BIN="$(bh_find_bro_headless "$PROJECT_DIR")" || {
    echo "  SKIP  bronze_host_observer  (bro-headless not built)"
    exit 77   # the automake convention for "skipped", not "passed"
}

MODULE="$(bh_ensure_module "$PROJECT_DIR" "$SCRIPT_DIR/appdir_observer" \
                           "$SCRIPT_DIR/apps/observer_probe.js")"
case $? in
    0)  ;;
    77) echo "  SKIP  bronze_host_observer  (no bronze CLI in this tree)"
        echo "        Configure with -DBRONZE_WITH_LLVM=ON to build one."
        exit 77 ;;
    *)  echo "  FAIL  bronze_host_observer  (observer_probe.js did not compile)"
        exit 1 ;;
esac

APP_DIR="$(bh_to_win_path "$SCRIPT_DIR/appdir_observer")"

RAW="$("$BIN" "$APP_DIR" -e "advanceTime($((FRAMES * 16)))" 2>&1)"
STATUS=$?
CLEAN="$(printf '%s\n' "$RAW" | tr -d '\r')"
ACTUAL="$(printf '%s\n' "$CLEAN" | grep '^APP ' || true)"

if [[ $STATUS -ne 0 ]]; then
    echo "  FAIL  bronze_host_observer  (exit $STATUS)"
    printf '%s\n' "$CLEAN" | tail -20 | sed 's/^/        /'
    exit 1
fi

if diff -u "$EXPECTED" <(printf '%s\n' "$ACTUAL") > /tmp/bronze_observer_diff.$$ 2>&1; then
    echo "  PASS  bronze_host_observer"
    rm -f /tmp/bronze_observer_diff.$$
    exit 0
fi

echo "  FAIL  bronze_host_observer  (output differs from the pinned expectation)"
sed 's/^/        /' /tmp/bronze_observer_diff.$$ | head -80
rm -f /tmp/bronze_observer_diff.$$
exit 1
