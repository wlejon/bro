#!/bin/bash
# Layout comparison: extract DOM properties from Chrome and bro, then diff.
# Usage: ./run.sh [case-name]
#   ./run.sh              # Run all cases
#   ./run.sh box-model    # Run single case
#
# Environment:
#   BRO_HEADLESS   Path to bro-headless (default: auto-detect from build/)
#   TOLERANCE      Numeric tolerance in px (default: 2)

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
CASES_DIR="$SCRIPT_DIR/cases"
OUTPUT_DIR="$SCRIPT_DIR/output"
TOLERANCE="${TOLERANCE:-2}"

# Find bro-headless
if [ -z "${BRO_HEADLESS:-}" ]; then
    # Try common locations
    for candidate in \
        "$SCRIPT_DIR/../../build/src/headless/Debug/bro-headless.exe" \
        "$SCRIPT_DIR/../../build/src/headless/Release/bro-headless.exe" \
        "$SCRIPT_DIR/../../build/src/headless/bro-headless"; do
        if [ -x "$candidate" ]; then
            BRO_HEADLESS="$candidate"
            break
        fi
    done
    if [ -z "${BRO_HEADLESS:-}" ]; then
        echo "ERROR: Cannot find bro-headless. Set BRO_HEADLESS env var."
        exit 1
    fi
fi

echo "bro-headless: $BRO_HEADLESS"
echo "Tolerance: ${TOLERANCE}px"
echo ""

# Ensure puppeteer is available
if ! node -e "require('puppeteer')" 2>/dev/null; then
    echo "Installing puppeteer..."
    npm install --no-save puppeteer 2>&1 | tail -1
    echo ""
fi

mkdir -p "$OUTPUT_DIR"

# Collect cases
if [ -n "${1:-}" ]; then
    CASES=("$1")
else
    CASES=()
    for d in "$CASES_DIR"/*/; do
        CASES+=("$(basename "$d")")
    done
fi

PASS=0
FAIL=0
ERRORS=0

for case_name in "${CASES[@]}"; do
    case_dir="$CASES_DIR/$case_name"
    if [ ! -f "$case_dir/index.html" ]; then
        echo "SKIP: $case_name (no index.html)"
        continue
    fi

    chrome_json="$OUTPUT_DIR/${case_name}.chrome.json"
    bro_json="$OUTPUT_DIR/${case_name}.bro.json"

    echo "--- $case_name ---"

    # Chrome extraction
    if ! node "$SCRIPT_DIR/chrome_extract.mjs" "$case_dir" "$chrome_json" 2>&1; then
        echo "  ERROR: Chrome extraction failed"
        ERRORS=$((ERRORS + 1))
        continue
    fi

    # Bro extraction — generate a wrapper script with the output path baked in
    bro_output_abs="$(cd "$OUTPUT_DIR" && pwd)/${case_name}.bro.json"
    bro_output_fwd="$(echo "$bro_output_abs" | sed 's/\\/\//g')"
    tmp_script="$OUTPUT_DIR/_bro_extract_tmp.js"
    {
        echo "var __BRO_OUTPUT = '$bro_output_fwd';"
        cat "$SCRIPT_DIR/bro_extract.js"
    } > "$tmp_script"

    if ! "$BRO_HEADLESS" --no-gpu --width 800 --height 600 "$case_dir" "$tmp_script" 2>&1; then
        echo "  ERROR: Bro extraction failed"
        ERRORS=$((ERRORS + 1))
        rm -f "$tmp_script"
        continue
    fi
    rm -f "$tmp_script"

    # Compare
    echo ""
    if node "$SCRIPT_DIR/compare.mjs" "$chrome_json" "$bro_json" --tolerance="$TOLERANCE"; then
        PASS=$((PASS + 1))
    else
        FAIL=$((FAIL + 1))
    fi
    echo ""
done

echo "==============================="
echo "Results: $PASS passed, $FAIL failed, $ERRORS errors (${#CASES[@]} total)"
echo "==============================="

exit $((FAIL + ERRORS > 0 ? 1 : 0))
