#!/bin/bash
# Build Skia for macOS from source (arm64 or x86_64).
#
# Prerequisites:
#   - Xcode Command Line Tools (clang, make, python3)
#   - ninja (brew install ninja)
#
# Ganesh is built for Vulkan only (bro has no GL); on macOS it runs on MoltenVK.
# The Vulkan headers come from Skia's own third_party tree, so no Vulkan SDK
# is needed to build the lib.
#
# On first run, this script clones Skia (~1 GB) into third_party/skia/src/
# and runs `python3 tools/git-sync-deps`, which downloads several hundred MB
# of additional build dependencies (including an Emscripten SDK). Expect the
# first build to take 15-25 minutes end-to-end on modern hardware; subsequent
# rebuilds reuse the cached source tree.
#
# Usage:
#   cd third_party/skia
#   ./build_skia_mac.sh          # builds Release
#   ./build_skia_mac.sh Debug    # builds Debug
#   ./build_skia_mac.sh all      # builds both
#
# SKIA_SRC=/path/to/checkout builds from a full Skia checkout elsewhere —
# third_party/skia/src normally holds the trimmed source bundle, which is not
# a buildable tree. Either way the checkout is pinned to SKIA_COMMIT, the
# chrome/m147 commit the source bundle was cut from.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
SKIA_SRC="${SKIA_SRC:-$SCRIPT_DIR/src}"
SKIA_COMMIT="abbe599fb3c0ef2fa82bfadbb0ddcd321f22faf0"   # chrome/m147
CONFIG="${1:-Release}"

# Derive target arch from *hardware*, not from the process arch. `uname -m`
# reports x86_64 when this script runs under a Rosetta shell on Apple Silicon,
# which would silently produce an x86_64 Skia that later fails to link against
# the arm64 rest of the build.
if [ "$(sysctl -n hw.optional.arm64 2>/dev/null || echo 0)" = "1" ]; then
    TARGET_CPU="arm64"
else
    case "$(uname -m)" in
        x86_64) TARGET_CPU="x64" ;;
        *) echo "Unsupported arch: $(uname -m)"; exit 1 ;;
    esac
fi

# Preflight: a damaged Xcode Command Line Tools install (common after
# Migration Assistant) can leave a gutted libc++ directory that shadows the
# real headers in the SDK, causing every C++ compile to fail with "'cstddef'
# file not found". Check once, fail fast, before burning 15 minutes of build
# time on the first PNG source.
if ! echo '#include <cstddef>' | clang++ -std=c++20 -x c++ -fsyntax-only - >/dev/null 2>&1; then
    echo "error: clang++ cannot find <cstddef>. Xcode Command Line Tools appear damaged."
    echo "  Try:  sudo rm -rf /Library/Developer/CommandLineTools && sudo xcode-select --install"
    exit 1
fi

build_config() {
    local config="$1"
    local is_debug="false"
    local is_official="true"
    local extra_cflags=""

    # Keep is_official_build=false in both configs — the "official" build
    # path skips the include wiring for bundled externals like libwebp, which
    # breaks compilation of SkWebpCodec. "is_debug" already selects the
    # optimization level and assertion visibility we care about.
    is_official="false"
    if [ "$config" = "Debug" ]; then
        is_debug="true"
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
        skia_use_gl=false
        skia_use_vulkan=true
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

    # We must force is_official_build=false above (for the bundled-libwebp
    # include wiring), which is the non-official path that embeds full debug
    # symbols — ballooning the Release archive to ~500 MB. The Linux/Windows
    # Release libs are official builds and ship lean (~20-40 MB). Strip the
    # debug symbols from the Release archive to match (keeps all global/external
    # symbols needed for linking; ~500 MB -> ~20 MB). Debug keeps its symbols.
    # Use the system tools explicitly so a non-Apple strip/ranlib on PATH
    # (Homebrew/conda) can't corrupt the Mach-O archive.
    if [ "$config" != "Debug" ]; then
        echo "=== Stripping debug symbols from Release libskia.a ==="
        /usr/bin/strip -S "$dest/libskia.a"
        /usr/bin/ranlib "$dest/libskia.a"
    fi
    echo "=== Installed libskia.a to $dest ==="
}

if [ ! -f "$SKIA_SRC/BUILD.gn" ]; then
    echo "=== Fetching Skia source ($SKIA_COMMIT) ==="
    mkdir -p "$SKIA_SRC"
    cd "$SKIA_SRC"
    if [ ! -d .git ]; then
        git init -q
        git remote add origin https://skia.googlesource.com/skia.git 2>/dev/null || true
    fi
    git fetch --depth 1 origin "$SKIA_COMMIT"
    git checkout -f FETCH_HEAD
fi

cd "$SKIA_SRC"
if [ "$(git rev-parse HEAD)" != "$SKIA_COMMIT" ]; then
    echo "error: $SKIA_SRC is at $(git rev-parse HEAD), expected $SKIA_COMMIT (chrome/m147)."
    echo "  The library must match the headers in the source bundle; check out $SKIA_COMMIT."
    exit 1
fi

echo "=== Syncing Skia dependencies ==="
python3 tools/git-sync-deps

if [ "$CONFIG" = "all" ]; then
    build_config Release
    build_config Debug
else
    build_config "$CONFIG"
fi

echo "=== Done. Run 'cmake -B build' from the project root. ==="
