#!/usr/bin/env bash
# Integration check for VideoEncoder and GifEncoder in the bronze host layer
# (src/bronze_host/host_video.cpp).
#
# WHAT ONLY THIS CHECK CATCHES.
#
#   * `refuse.*` — the KIND of every refusal. TypeError for the wrong sort of
#     argument, RangeError for the wrong size, Error for a state problem. An
#     app branches on `e.name`, so a layer that answered plain Error to all
#     three would compile, run, write correct files, and quietly take away the
#     one distinction that lets a caller recover. Nothing but an explicit
#     assertion can notice that.
#
#   * `canvas.*` — encoding a 2D canvas that the PAGE created. The bronze
#     host's own canvas has no 2D context (getContext answers only webgl /
#     webgl2), so this is the only shape the case has: interpreted UI, compiled
#     recorder. It is also the only path here that crosses realms.
#
#   * `gif.frames` / `vp.size` — the viewport capture, which for a compiled app
#     is not one option among several but the only one that can see a WebGL
#     render. `vp.size` is pinned at bro-headless's own 1920x1080 default —
#     which it assigns over the appdir's bro.json, so the fixture's 640x480 is
#     dead weight and the size is the same on every machine.
#
#   * `enc.frames0` / `enc.frames1` — framesWritten before and after finish().
#     libvpx buffers, so the number only means "everything I pushed" after the
#     flush; pinning both is what keeps a future change from making the
#     pre-flush number look meaningful.
#
# THE FILES are checked here rather than in the probe, because the probe has no
# filesystem: this layer gives compiled code no fs module, deliberately. So the
# runner runs the app in a scratch directory (the encoders resolve a relative
# path against the process's cwd) and asserts afterwards that each file exists
# and is not a bare header.
#
# WHAT THIS CHECK DOES NOT COVER: that the files DECODE. Byte-level muxing is
# already pinned by tests/video/*, which drives the same two encoders through
# bro's own bindings — this layer adds argument handling and refusals on top of
# them, and that is what is asserted here. A second copy of the muxer's
# expectations would drift from the first without ever catching anything.
#
# Everything else follows run_parser_test.sh: exit 0 pass, 1 fail, 77 skip.
#
# Usage:
#   tests/bronze_host/run_video_test.sh
#
# Environment:
#   BRO_HEADLESS  path to bro-headless (default: found under build/)
#   BRONZE        path to the bronze CLI (default: found under build/)

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/../.." && pwd)"
EXPECTED="$SCRIPT_DIR/expected/video_probe.expected"

source "$SCRIPT_DIR/lib.sh"

BIN="$(bh_find_bro_headless "$PROJECT_DIR")" || {
    echo "  SKIP  bronze_host_video  (bro-headless not built)"
    exit 77   # the automake convention for "skipped", not "passed"
}

MODULE="$(bh_ensure_module "$PROJECT_DIR" "$SCRIPT_DIR/appdir_video" \
                           "$SCRIPT_DIR/apps/video_probe.js")"
case $? in
    0)  ;;
    77) echo "  SKIP  bronze_host_video  (no bronze CLI in this tree)"
        echo "        Configure with -DBRONZE_WITH_LLVM=ON to build one."
        exit 77 ;;
    *)  echo "  FAIL  bronze_host_video  (video_probe.js did not compile)"
        exit 1 ;;
esac

APP_DIR="$(bh_to_win_path "$SCRIPT_DIR/appdir_video")"

# The scratch directory the encoders write into. Everything the probe names is
# a bare filename, so this is what "relative to the cwd" resolves to — and it
# means a half-finished run leaves its debris here and nowhere near the tree.
OUT_DIR="$(mktemp -d)"
trap 'rm -rf "$OUT_DIR"' EXIT

RAW="$(cd "$OUT_DIR" && "$BIN" "$APP_DIR" -e "advanceTime(64)" 2>&1)"
STATUS=$?
CLEAN="$(printf '%s\n' "$RAW" | tr -d '\r')"
ACTUAL="$(printf '%s\n' "$CLEAN" | grep '^APP ' || true)"

if [[ $STATUS -ne 0 ]]; then
    echo "  FAIL  bronze_host_video  (exit $STATUS)"
    printf '%s\n' "$CLEAN" | tail -20 | sed 's/^/        /'
    exit 1
fi

if ! diff -u "$EXPECTED" <(printf '%s\n' "$ACTUAL") > /tmp/bronze_video_diff.$$ 2>&1; then
    echo "  FAIL  bronze_host_video  (output differs from the pinned expectation)"
    sed 's/^/        /' /tmp/bronze_video_diff.$$ | head -80
    rm -f /tmp/bronze_video_diff.$$
    exit 1
fi
rm -f /tmp/bronze_video_diff.$$

# The half the probe cannot assert. A size floor rather than an exact byte
# count: what is being checked is that a real file was muxed and closed, and an
# exact size would pin libvpx's output for no benefit — tests/video/ owns the
# muxing. The floors are "bigger than an empty container's header".
check_file() {
    local name="$1" floor="$2" path="$OUT_DIR/$1"
    if [[ ! -f "$path" ]]; then
        echo "  FAIL  bronze_host_video  ($name was never written)"
        return 1
    fi
    local size
    size="$(wc -c < "$path" | tr -d ' ')"
    if (( size < floor )); then
        echo "  FAIL  bronze_host_video  ($name is $size bytes, expected > $floor)"
        return 1
    fi
    return 0
}

check_file bronze_video_probe.webm 200 || exit 1   # 3 frames of 64x48
check_file bronze_video_audio.webm 200 || exit 1   # 200 ms of Opus, no picture
check_file bronze_video_canvas.webm 100 || exit 1  # one frame
check_file bronze_video_probe.gif 200 || exit 1    # 3 viewport frames

# The odd-width encoder never opened a file, because the config was refused
# before one was created. A file here would mean the refusal came too late.
if [[ -e "$OUT_DIR/bronze_video_odd.webm" ]]; then
    echo "  FAIL  bronze_host_video  (a refused config still created a file)"
    exit 1
fi

echo "  PASS  bronze_host_video"
exit 0
