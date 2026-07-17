#!/usr/bin/env bash
# Test runner for bro integration tests.
# Usage: ./tests/run_tests.sh [filter]
#   filter: optional substring to match test file paths (e.g. "dom" or "click")
#
# Discovers all tests/*/test_*.js files, runs each via bro-headless, and reports
# pass/fail with a summary. Runs on the GPU path (headless's default) so the
# tests exercise the same renderer, WebGL, and layer compositing that ship —
# CPU-only raster is a different code path and would leave those untested.

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
TEST_APP="$SCRIPT_DIR/test_app"

# Find the headless binary. BRO_HEADLESS overrides auto-detection so the suite
# can run against an arbitrary build dir or a packaged dist binary.
if [[ -n "${BRO_HEADLESS:-}" ]]; then
    BRO="$BRO_HEADLESS"
    if [[ ! -x "$BRO" ]]; then
        echo "ERROR: BRO_HEADLESS=$BRO is not an executable"
        exit 1
    fi
elif [[ -f "$PROJECT_DIR/build/Debug/bro-headless.exe" ]]; then
    BRO="$PROJECT_DIR/build/Debug/bro-headless.exe"
elif [[ -f "$PROJECT_DIR/build/Release/bro-headless.exe" ]]; then
    BRO="$PROJECT_DIR/build/Release/bro-headless.exe"
elif [[ -f "$PROJECT_DIR/build/bro-headless" ]]; then
    BRO="$PROJECT_DIR/build/bro-headless"
elif [[ -f "$PROJECT_DIR/build-release/bro-headless" ]]; then
    BRO="$PROJECT_DIR/build-release/bro-headless"
else
    echo "ERROR: bro-headless not found. Build first with: cmake --build build"
    exit 1
fi

FILTER="${1:-}"

# Per-test timeout so one hung test can't wedge the whole suite (or CI).
# Override with BRO_TEST_TIMEOUT (seconds). Uses coreutils `timeout` when
# available (git-bash and Linux have it; stock macOS may not — fall back to
# running the test bare there).
TEST_TIMEOUT="${BRO_TEST_TIMEOUT:-120}"
TIMEOUT_BIN=""
if command -v timeout >/dev/null 2>&1; then
    TIMEOUT_BIN="timeout"
fi

# Collect test files
mapfile -t TEST_FILES < <(find "$SCRIPT_DIR" -path "*/test_app" -prune -o -name "test_*.js" -print | sort)

if [[ ${#TEST_FILES[@]} -eq 0 ]]; then
    echo "No test files found."
    exit 1
fi

PASSED=0
FAILED=0
ERRORS=()

for TEST_FILE in "${TEST_FILES[@]}"; do
    # Relative path for display
    REL="${TEST_FILE#$SCRIPT_DIR/}"

    # Apply filter
    if [[ -n "$FILTER" && "$REL" != *"$FILTER"* ]]; then
        continue
    fi

    # Run the test (under a per-test timeout when `timeout` exists)
    if [[ -n "$TIMEOUT_BIN" ]]; then
        OUTPUT=$("$TIMEOUT_BIN" -k 10 "$TEST_TIMEOUT" "$BRO" "$TEST_APP" "$TEST_FILE" 2>&1)
        STATUS=$?
    else
        OUTPUT=$("$BRO" "$TEST_APP" "$TEST_FILE" 2>&1)
        STATUS=$?
    fi

    if [[ $STATUS -eq 0 ]]; then
        echo "  PASS  $REL"
        ((PASSED++))
    elif [[ -n "$TIMEOUT_BIN" && ($STATUS -eq 124 || $STATUS -eq 137) ]]; then
        # 124 = timeout sent TERM, 137 = timeout escalated to KILL
        echo "  FAIL  $REL  (TIMEOUT after ${TEST_TIMEOUT}s)"
        echo "$OUTPUT" | tail -20 | sed 's/^/        /'
        ((FAILED++))
        ERRORS+=("$REL (TIMEOUT)")
    else
        echo "  FAIL  $REL"
        # Show first 20 lines of output for diagnosis
        echo "$OUTPUT" | head -20 | sed 's/^/        /'
        ((FAILED++))
        ERRORS+=("$REL")
    fi
done

TOTAL=$((PASSED + FAILED))
echo ""
echo "────────────────────────────────────"
echo "  $TOTAL tests: $PASSED passed, $FAILED failed"

if [[ $FAILED -gt 0 ]]; then
    echo ""
    echo "  Failed:"
    for E in "${ERRORS[@]}"; do
        echo "    - $E"
    done
    echo "────────────────────────────────────"
    exit 1
fi

echo "────────────────────────────────────"
