#!/bin/bash
# Build Skia for macOS from source (arm64 or x86_64).
#
# Prerequisites:
#   - Xcode Command Line Tools (clang, make, python3)
#   - ninja (brew install ninja)
#
# Usage:
#   cd third_party/skia
#   ./build_skia_mac.sh          # builds Release
#   ./build_skia_mac.sh Debug    # builds Debug
#   ./build_skia_mac.sh all      # builds both

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
SKIA_SRC="$SCRIPT_DIR/src"
CONFIG="${1:-Release}"

ARCH="$(uname -m)"
case "$ARCH" in
    arm64)  TARGET_CPU="arm64" ;;
    x86_64) TARGET_CPU="x64" ;;
    *) echo "Unsupported arch: $ARCH"; exit 1 ;;
esac

build_config() {
    local config="$1"
    local is_debug="false"
    local is_official="true"
    local extra_cflags=""

    if [ "$config" = "Debug" ]; then
        is_debug="true"
        is_official="false"
        extra_cflags='extra_cflags=["-g"]'
    else
        extra_cflags='extra_cflags=["-DNDEBUG"]'
    fi

    echo "=== Building Skia ($config, $TARGET_CPU) ==="

    cd "$SKIA_SRC"

    local args="
        is_official_build=$is_official
        is_debug=$is_debug
        target_cpu=\"$TARGET_CPU\"
        skia_use_metal=false
        skia_use_gl=true
        skia_enable_ganesh=true
        skia_enable_svg=true
        skia_use_expat=true
        skia_use_fontconfig=false
        skia_use_freetype=false
        skia_use_system_harfbuzz=false
        skia_use_dng_sdk=false
        skia_use_piex=false
        skia_use_libjpeg_turbo_decode=true
        skia_use_libjpeg_turbo_encode=true
        skia_use_libpng_decode=true
        skia_use_libpng_encode=true
        skia_use_libwebp_decode=true
        skia_use_libwebp_encode=false
        skia_use_wuffs=true
        cc=\"clang\"
        cxx=\"clang++\"
        extra_cflags_cc=[\"-frtti\"]
        $extra_cflags
    "

    bin/gn gen "out/$config" --args="$args"
    ninja -C "out/$config" skia

    local dest="$SCRIPT_DIR/lib/$config"
    mkdir -p "$dest"
    cp "out/$config/libskia.a" "$dest/"
    echo "=== Installed libskia.a to $dest ==="
}

if [ ! -d "$SKIA_SRC" ]; then
    echo "=== Cloning Skia source ==="
    git clone https://skia.googlesource.com/skia.git "$SKIA_SRC"
fi

echo "=== Syncing Skia dependencies ==="
cd "$SKIA_SRC"
python3 tools/git-sync-deps

if [ "$CONFIG" = "all" ]; then
    build_config Release
    build_config Debug
else
    build_config "$CONFIG"
fi

echo "=== Done. Run 'cmake -B build' from the project root. ==="
