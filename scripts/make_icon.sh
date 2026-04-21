#!/usr/bin/env bash
# Regenerate system/icon.png from scripts/make_icon/index.html using the
# GPU headless renderer (same Skia pipeline as windowed mode).
#
# Usage: scripts/make_icon.sh [--build-dir build] [--config Release]
set -euo pipefail

BUILD_DIR="build"
CONFIG="Release"

while [[ $# -gt 0 ]]; do
    case "$1" in
        --build-dir) BUILD_DIR="$2"; shift 2 ;;
        --config)    CONFIG="$2";    shift 2 ;;
        -h|--help)   sed -n '2,6p' "$0"; exit 0 ;;
        *) echo "unknown arg: $1" >&2; exit 1 ;;
    esac
done

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$REPO_ROOT"

case "$(uname -s)" in
    MINGW*|MSYS*|CYGWIN*) HEADLESS="$BUILD_DIR/src/headless/$CONFIG/bro-headless.exe" ;;
    *)                    HEADLESS="$BUILD_DIR/src/headless/bro-headless" ;;
esac

if [[ ! -x "$HEADLESS" ]]; then
    echo "error: $HEADLESS not built. Run: cmake --build $BUILD_DIR --config $CONFIG" >&2
    exit 1
fi

# Viewport is larger than the 256x256 icon so the engine's menu-bar strip
# does not land inside the cropped #icon element. advanceTime(3000) ticks
# past the splash dismiss (1800ms idle + 500ms fade) before screenshotting.
"$HEADLESS" --width 384 --height 384 scripts/make_icon \
    -e "bro.menu.set([])" \
    -e "advanceTime(3000)" \
    -e "screenshot('system/icon.png', '#icon')"

echo "Wrote system/icon.png"
