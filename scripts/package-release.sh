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

# Each target lives in its own subdirectory (src/, src/headless/, src/server/).
# On Windows (multi-config), the config suffix is appended to each.
# Resolve per-binary paths via a helper.
bin_path() {
    local target="$1"
    local subdir="$2"
    if [[ "$PLATFORM" == "win" ]]; then
        echo "$BUILD_DIR/src/$subdir$CONFIG/$target$EXE"
    else
        echo "$BUILD_DIR/src/$subdir$target$EXE"
    fi
}

BRO_EXE="$(bin_path bro '')"
BRO_HEADLESS_EXE="$(bin_path bro-headless 'headless/')"
BRO_SERVER_EXE="$(bin_path bro-server 'server/')"

if [[ ! -x "$BRO_EXE" ]]; then
    echo "error: $BRO_EXE not found. Build first:" >&2
    echo "  cmake --build $BUILD_DIR --config $CONFIG" >&2
    exit 1
fi

OUT_NAME="bro-${VERSION}-${PLATFORM}-${ARCH}"
OUT_DIR="dist/$OUT_NAME"

echo ">>> Packaging $OUT_NAME"
rm -rf "$OUT_DIR"
mkdir -p "$OUT_DIR/apps"

# --- Executables -----------------------------------------------------------
for src in "$BRO_EXE" "$BRO_HEADLESS_EXE" "$BRO_SERVER_EXE"; do
    if [[ -x "$src" ]]; then
        cp "$src" "$OUT_DIR/"
    else
        echo "warning: $src not found, skipping" >&2
    fi
done

# --- Shared libraries next to exes ----------------------------------------
# Windows: *.dll copied by CMake next to the exe. Mac/Linux: any *.dylib/*.so.
# Search all three bin dirs since each target has its own output directory.
shopt -s nullglob
for bin in "$BRO_EXE" "$BRO_HEADLESS_EXE" "$BRO_SERVER_EXE"; do
    bin_dir="$(dirname "$bin")"
    for lib in $bin_dir/$LIB_GLOB; do
        cp -a "$lib" "$OUT_DIR/"
    done
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
if [[ "$PLATFORM" == "macos" ]]; then
    cat > "$OUT_DIR/README.txt" <<EOF
Bro ${VERSION} (${PLATFORM}-${ARCH})

Run:
  open Bro.app                              # launcher (double-clickable)
  Bro.app/Contents/MacOS/bro apps/tetris    # specific app from terminal
  ./bro-headless apps/example test.js       # CLI, headless mode

Unsigned build
--------------
This bundle isn't codesigned or notarized, so macOS Gatekeeper will
quarantine it after download. If you see "Bro.app is damaged" or
"cannot be opened because the developer cannot be verified":

  right-click Bro.app -> Open  (one-time bypass)

or strip the quarantine flag:

  xattr -dr com.apple.quarantine Bro.app
EOF
else
    cat > "$OUT_DIR/README.txt" <<EOF
Bro ${VERSION} (${PLATFORM}-${ARCH})

Run:
  ./bro${EXE}                    # opens the launcher
  ./bro${EXE} apps/tetris        # runs a specific app
  ./bro-headless${EXE} apps/example test.js
EOF
fi

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

# --- macOS .app bundle ----------------------------------------------------
# Finder launches a Mach-O binary in Terminal; wrap everything in a bundle so
# double-click opens the app with no terminal window. Binary + apps + system
# all live under Contents/MacOS so main.cpp's chdir(exeDir()) picks them up.
if [[ "$PLATFORM" == "macos" ]]; then
    APP="$OUT_DIR/Bro.app"
    rm -rf "$APP"
    mkdir -p "$APP/Contents/MacOS"
    # Move the GUI binary and its runtime data into the bundle. Keep the CLI
    # tools (bro-headless, bro-server) alongside Bro.app so users can invoke
    # them from a terminal without reaching into the bundle.
    CLI_KEEP=(bro-headless bro-server README.txt LICENSE)
    for item in "$OUT_DIR"/*; do
        name="$(basename "$item")"
        [[ "$name" == "Bro.app" ]] && continue
        keep=0
        for k in "${CLI_KEEP[@]}"; do [[ "$name" == "$k" ]] && keep=1 && break; done
        [[ $keep -eq 1 ]] && continue
        mv "$item" "$APP/Contents/MacOS/"
    done
    cat > "$APP/Contents/Info.plist" <<PLIST
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <key>CFBundleExecutable</key>        <string>bro</string>
    <key>CFBundleIdentifier</key>        <string>io.novahorizons.bro</string>
    <key>CFBundleName</key>              <string>Bro</string>
    <key>CFBundleDisplayName</key>       <string>Bro</string>
    <key>CFBundleVersion</key>           <string>${VERSION}</string>
    <key>CFBundleShortVersionString</key><string>${VERSION}</string>
    <key>CFBundlePackageType</key>       <string>APPL</string>
    <key>CFBundleSignature</key>         <string>????</string>
    <key>LSMinimumSystemVersion</key>    <string>11.0</string>
    <key>NSHighResolutionCapable</key>   <true/>
    <key>NSSupportsAutomaticGraphicsSwitching</key><true/>
</dict>
</plist>
PLIST
fi

# --- Archive --------------------------------------------------------------
# macOS: ditto preserves extended attributes, resource forks, and symlinks
# inside .app bundles — `zip -r` can corrupt those. Elsewhere, plain zip.
ARCHIVE="dist/${OUT_NAME}.zip"
rm -f "$ARCHIVE"
echo ""
echo ">>> Archiving $ARCHIVE"
if [[ "$PLATFORM" == "macos" ]]; then
    (cd dist && ditto -c -k --keepParent "$OUT_NAME" "${OUT_NAME}.zip")
else
    (cd dist && zip -rq "${OUT_NAME}.zip" "$OUT_NAME")
fi

# --- Report ---------------------------------------------------------------
echo ""
echo "Staged: $OUT_DIR"
if command -v du >/dev/null 2>&1; then
    du -sh "$OUT_DIR" | awk '{print "Staged size:  " $1}'
    du -sh "$ARCHIVE" | awk '{print "Archive size: " $1}'
fi
echo ""
echo "Next: verify by running"
if [[ "$PLATFORM" == "macos" ]]; then
    echo "  open $OUT_DIR/Bro.app"
    echo "  $OUT_DIR/bro-headless apps/example   # CLI tools stay outside the bundle"
else
    echo "  (cd $OUT_DIR && ./bro$EXE)"
fi
echo ""
echo "Upload $ARCHIVE to your GitHub release."
