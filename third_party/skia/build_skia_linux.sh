#!/bin/bash
# Build Skia for Linux from source.
#
# Prerequisites (Debian/Ubuntu):
#   sudo apt install build-essential clang python3 ninja-build \
#                    libfreetype-dev libfontconfig-dev libgl-dev
#
# Usage:
#   cd third_party/skia
#   ./build_skia_linux.sh          # builds Release
#   ./build_skia_linux.sh Debug    # builds Debug
#   ./build_skia_linux.sh all      # builds both

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
SKIA_SRC="$SCRIPT_DIR/src"
CONFIG="${1:-Release}"

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

    echo "=== Building Skia ($config) ==="

    cd "$SKIA_SRC"

    local args="
        is_official_build=$is_official
        is_debug=$is_debug
        skia_use_freetype=true
        skia_use_fontconfig=true
        skia_use_system_freetype2=true
        skia_use_system_harfbuzz=false
        skia_use_gl=true
        skia_enable_ganesh=true
        skia_enable_svg=true
        skia_use_expat=true
        skia_use_x11=false
        skia_use_dng_sdk=false
        skia_use_piex=false
        skia_use_system_libjpeg_turbo=true
        skia_use_system_libpng=true
        skia_use_system_libwebp=true
        skia_use_wuffs=true
        skia_use_libwebp_encode=false
        skia_use_libwebp_decode=true
        cc=\"clang\"
        cxx=\"clang++\"
        extra_cflags_cc=[\"-frtti\"]
        $extra_cflags
    "

    bin/gn gen "out/$config" --args="$args"
    ninja -C "out/$config" skia

    # Place the built library where CMake expects it
    local dest="$SCRIPT_DIR/lib/$config"
    mkdir -p "$dest"
    cp "out/$config/libskia.a" "$dest/"
    echo "=== Installed libskia.a to $dest ==="
}

# Clone Skia source if not present. Check for a tracked file rather than the
# directory itself — Docker bind mounts / named volumes create an empty dir
# that would otherwise fool the check. Clone-in-place (git init + fetch) so
# the existing mount point is reused without the "destination not empty" error.
if [ ! -f "$SKIA_SRC/BUILD.gn" ]; then
    echo "=== Cloning Skia source ==="
    mkdir -p "$SKIA_SRC"
    cd "$SKIA_SRC"
    if [ ! -d .git ]; then
        git init -q
        git remote add origin https://skia.googlesource.com/skia.git 2>/dev/null || true
    fi
    git fetch --depth 1 origin main
    git checkout -f FETCH_HEAD
    cd - >/dev/null
fi

# Sync dependencies. git-sync-deps fires ~40 parallel fetches at
# googlesource.com, which rate-limits aggressively. Force HTTP/1.1 + big
# post buffer, patch the script to serialize fetches, and retry with
# exponential backoff on transient transport errors.
echo "=== Syncing Skia dependencies ==="
cd "$SKIA_SRC"
git config --global http.version HTTP/1.1
git config --global http.postBuffer 524288000

# Replace the multithread() call with a serial loop so fetches go one at a
# time. Idempotent: only patches if the original form is still there.
if grep -q 'multithread(git_checkout_to_directory, list_of_arg_lists)' tools/git-sync-deps; then
    sed -i 's|multithread(git_checkout_to_directory, list_of_arg_lists)|[git_checkout_to_directory(*a) for a in list_of_arg_lists]|' tools/git-sync-deps
fi

delay=10
for attempt in 1 2 3 4 5; do
    if python3 tools/git-sync-deps; then
        break
    fi
    echo "=== git-sync-deps attempt $attempt failed; sleeping ${delay}s ==="
    sleep "$delay"
    delay=$((delay * 2))
done
python3 tools/git-sync-deps  # final check — fails build if still broken

if [ "$CONFIG" = "all" ]; then
    build_config Release
    build_config Debug
else
    build_config "$CONFIG"
fi

echo "=== Done. Run 'cmake -B build' from the project root. ==="
