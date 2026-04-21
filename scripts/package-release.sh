#!/usr/bin/env bash
# Stage a release directory for zipping and uploading to GitHub.
#
# Usage:
#   scripts/package-release.sh [--version X.Y.Z] [--build-dir build] [--config Release]
#
# Output: dist/bro-<version>-<platform>-<arch>/
# Zip it yourself after verifying contents.

set -euo pipefail

VERSION=""
BUILD_DIR="build"
CONFIG="Release"

while [[ $# -gt 0 ]]; do
    case "$1" in
        --version) VERSION="$2"; shift 2 ;;
        --build-dir) BUILD_DIR="$2"; shift 2 ;;
        --config) CONFIG="$2"; shift 2 ;;
        -h|--help)
            sed -n '2,10p' "$0"; exit 0 ;;
        *) echo "unknown arg: $1" >&2; exit 1 ;;
    esac
done

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$REPO_ROOT"

if [[ -z "$VERSION" ]]; then
    VERSION="$(git describe --tags --always --dirty 2>/dev/null || echo 0.0.0)"
fi

# Platform detection
case "$(uname -s)" in
    MINGW*|MSYS*|CYGWIN*) PLATFORM="win" ; EXE=".exe" ; LIB_GLOB="*.dll" ;;
    Darwin)               PLATFORM="macos" ; EXE=""     ; LIB_GLOB="*.dylib" ;;
    Linux)                PLATFORM="linux" ; EXE=""     ; LIB_GLOB="*.so*" ;;
    *) echo "unsupported platform: $(uname -s)" >&2; exit 1 ;;
esac

case "$(uname -m)" in
    x86_64|amd64)       ARCH="x64" ;;
    arm64|aarch64)      ARCH="arm64" ;;
    *)                  ARCH="$(uname -m)" ;;
esac

# On Windows (multi-config), binaries live in <build>/src/<Config>/.
# On mac/linux (single-config), they live in <build>/src/.
if [[ "$PLATFORM" == "win" ]]; then
    BIN_DIR="$BUILD_DIR/src/$CONFIG"
else
    BIN_DIR="$BUILD_DIR/src"
fi

if [[ ! -x "$BIN_DIR/bro$EXE" ]]; then
    echo "error: $BIN_DIR/bro$EXE not found. Build first:" >&2
    echo "  cmake --build $BUILD_DIR --config $CONFIG" >&2
    exit 1
fi

OUT_NAME="bro-${VERSION}-${PLATFORM}-${ARCH}"
OUT_DIR="dist/$OUT_NAME"

echo ">>> Packaging $OUT_NAME"
rm -rf "$OUT_DIR"
mkdir -p "$OUT_DIR/apps"

# --- Executables -----------------------------------------------------------
for exe in bro bro-headless bro-server; do
    cp "$BIN_DIR/$exe$EXE" "$OUT_DIR/"
done

# --- Shared libraries next to exes ----------------------------------------
# Windows: *.dll copied by CMake next to the exe. Mac/Linux: any *.dylib/*.so.
shopt -s nullglob
for lib in $BIN_DIR/$LIB_GLOB; do
    cp -a "$lib" "$OUT_DIR/"
done
shopt -u nullglob

# --- Root bro.json: double-click bro -> launcher --------------------------
cat > "$OUT_DIR/bro.json" <<'JSON'
{
    "app": "apps/launcher",
    "title": "Bro",
    "width": 1100,
    "height": 720
}
JSON

# --- README + LICENSE -----------------------------------------------------
cp LICENSE "$OUT_DIR/"
cat > "$OUT_DIR/README.txt" <<EOF
Bro ${VERSION} (${PLATFORM}-${ARCH})

Run:
  ./bro${EXE}              # opens the launcher
  ./bro${EXE} apps/tetris  # runs a specific app
  ./bro-headless${EXE} apps/example test.js

Source: https://github.com/jonnybro/bro
EOF

# --- system/ (global system panels: splash, menu, nav, perf, settings) ---
# Resolved by the engine as a relative path from cwd, which is the release
# root when bro.exe is launched from its own folder.
cp -a system "$OUT_DIR/"

# --- Apps -----------------------------------------------------------------
# Copy the launcher and every app it references, excluding dev artifacts.
APPS=(launcher $(python3 -c "
import json, sys
with open('apps/launcher/apps.json') as f:
    print(' '.join(a['dir'] for a in json.load(f)['apps']))
" 2>/dev/null || node -e "
const m = require('./apps/launcher/apps.json');
console.log(m.apps.map(a=>a.dir).join(' '));
"))

# Exclude patterns: dev tests, scene-editor screenshots, transient caches.
EXCLUDES=(
    --exclude='test_*.js'
    --exclude='tests'
    --exclude='node_modules'
    --exclude='.cache'
    --exclude='.bro_settings.json'
    --exclude='_*.png'
    --exclude='.DS_Store'
)

for app in "${APPS[@]}"; do
    src="apps/$app"
    dst="$OUT_DIR/apps/$app"
    if [[ ! -d "$src" ]]; then
        echo "warning: $src not found, skipping" >&2
        continue
    fi
    mkdir -p "$dst"
    # rsync if available, else tar-pipe fallback.
    if command -v rsync >/dev/null 2>&1; then
        rsync -a "${EXCLUDES[@]}" "$src/" "$dst/"
    else
        (cd "$src" && tar --exclude='test_*.js' --exclude='tests' \
            --exclude='node_modules' --exclude='.cache' \
            --exclude='.bro_settings.json' --exclude='_*.png' \
            --exclude='.DS_Store' -cf - .) | (cd "$dst" && tar -xf -)
    fi
done

# --- Report ---------------------------------------------------------------
echo ""
echo "Staged: $OUT_DIR"
if command -v du >/dev/null 2>&1; then
    du -sh "$OUT_DIR" | awk '{print "Size:  " $1}'
fi
echo ""
echo "Next: verify by running"
echo "  (cd $OUT_DIR && ./bro$EXE)"
echo "then zip:"
echo "  (cd dist && zip -r $OUT_NAME.zip $OUT_NAME)"
