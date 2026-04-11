#!/usr/bin/env bash
# Test runner for bro integration tests.
# Usage: ./tests/run_tests.sh [filter]
#   filter: optional substring to match test file paths (e.g. "dom" or "click")
#
# Discovers all tests/*/test_*.js files, runs each via bro-headless --no-gpu,
# and reports pass/fail with a summary.

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
TEST_APP="$SCRIPT_DIR/test_app"

# Find the headless binary
if [[ -f "$PROJECT_DIR/build/src/headless/Debug/bro-headless.exe" ]]; then
    BRO="$PROJECT_DIR/build/src/headless/Debug/bro-headless.exe"
elif [[ -f "$PROJECT_DIR/build/src/headless/Release/bro-headless.exe" ]]; then
    BRO="$PROJECT_DIR/build/src/headless/Release/bro-headless.exe"
elif [[ -f "$PROJECT_DIR/build/src/headless/bro-headless" ]]; then
    BRO="$PROJECT_DIR/build/src/headless/bro-headless"
else
    echo "ERROR: bro-headless not found. Build first with: cmake --build build"
    exit 1
fi

FILTER="${1:-}"

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

    # Run the test
    if OUTPUT=$("$BRO" --no-gpu "$TEST_APP" "$TEST_FILE" 2>&1); then
        echo "  PASS  $REL"
        ((PASSED++))
    else
        echo "  FAIL  $REL"
        # Show first 10 lines of output for diagnosis
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
