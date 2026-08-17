#!/usr/bin/env bash
# Integration check for the AUDIO surface of the bronze host layer
# (src/bronze_host/host_audio.cpp): boot bro-headless on appdir_audio, let the
# compiled probe create AudioContext, GainNode, OscillatorNode, BiquadFilterNode,
# AnalyserNode, AudioBuffer, AudioBufferSourceNode, and test procedural decodeAudioData.
#
# Usage:
#   tests/bronze_host/run_audio_test.sh
#
# Environment:
#   BRO_HEADLESS       path to bro-headless (default: found under build/)
#   BRONZE             path to the bronze CLI (default: found under build/)
#   BRO_BRONZE_FRAMES  frames to advance (default 8)

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/../.." && pwd)"
EXPECTED="$SCRIPT_DIR/expected/audio_probe.expected"
FRAMES="${BRO_BRONZE_FRAMES:-8}"

source "$SCRIPT_DIR/lib.sh"

BIN="$(bh_find_bro_headless "$PROJECT_DIR")" || {
    echo "  SKIP  bronze_host_audio  (bro-headless not built)"
    exit 77   # the automake convention for "skipped", not "passed"
}

MODULE="$(bh_ensure_module "$PROJECT_DIR" "$SCRIPT_DIR/appdir_audio" \
                           "$SCRIPT_DIR/apps/audio_probe.js")"
case $? in
    0)  ;;
    77) echo "  SKIP  bronze_host_audio  (no bronze CLI in this tree)"
        echo "        Configure with -DBRONZE_WITH_LLVM=ON to build one."
        exit 77 ;;
    *)  echo "  FAIL  bronze_host_audio  (audio_probe.js did not compile)"
        exit 1 ;;
esac

APP_DIR="$(bh_to_win_path "$SCRIPT_DIR/appdir_audio")"

RAW="$("$BIN" "$APP_DIR" -e "advanceTime($((FRAMES * 16)))" 2>&1)"
STATUS=$?
CLEAN="$(printf '%s\n' "$RAW" | tr -d '\r')"
ACTUAL="$(printf '%s\n' "$CLEAN" | grep '^APP ' || true)"

if [[ $STATUS -ne 0 ]]; then
    echo "  FAIL  bronze_host_audio  (exit $STATUS)"
    printf '%s\n' "$CLEAN" | tail -20 | sed 's/^/        /'
    exit 1
fi

if diff -u "$EXPECTED" <(printf '%s\n' "$ACTUAL") > /tmp/bronze_audio_diff.$$ 2>&1; then
    echo "  PASS  bronze_host_audio"
    rm -f /tmp/bronze_audio_diff.$$
    exit 0
fi

echo "  FAIL  bronze_host_audio  (output differs from the pinned expectation)"
sed 's/^/        /' /tmp/bronze_audio_diff.$$ | head -80
rm -f /tmp/bronze_audio_diff.$$
exit 1
