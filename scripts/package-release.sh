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

# On single-config generators (Ninja/Make), --config is ignored; the build dir
# is locked to whatever CMAKE_BUILD_TYPE was set at configure time. Catch the
# common Linux/macOS mistake of pointing this script at a Debug build dir.
if [[ -f "$BUILD_DIR/CMakeCache.txt" ]]; then
    # `|| true`: a Visual Studio multi-config cache has no CMAKE_BUILD_TYPE, so
    # this grep legitimately matches nothing — without the guard, `set -e` kills
    # the whole script on the empty command substitution.
    CACHED_TYPE="$(grep -E '^CMAKE_BUILD_TYPE:STRING=' "$BUILD_DIR/CMakeCache.txt" | cut -d= -f2 || true)"
    CACHED_GEN="$(grep -E '^CMAKE_GENERATOR:INTERNAL=' "$BUILD_DIR/CMakeCache.txt" | cut -d= -f2 || true)"
    if [[ "$CACHED_GEN" != "Visual Studio"* ]] && [[ -n "$CACHED_TYPE" ]] && [[ "$CACHED_TYPE" != "Release" ]] && [[ "$CACHED_TYPE" != "RelWithDebInfo" ]] && [[ "$CACHED_TYPE" != "MinSizeRel" ]]; then
        echo "ERROR: $BUILD_DIR is configured as $CACHED_TYPE, not Release." >&2
        echo "  Configure a separate Release build dir:" >&2
        echo "    cmake -B build-release -DCMAKE_BUILD_TYPE=Release" >&2
        echo "    cmake --build build-release" >&2
        echo "    $0 --build-dir build-release${VERSION:+ --version $VERSION}" >&2
        exit 1
    fi
fi

# Platform detection: prefer the *build's* generator over the host shell's
# uname so this works whether you run it from git-bash, WSL, or Cygwin against
# the same Windows build. WSL bash on Windows reports `uname -s = Linux` but
# the binaries it's packaging are .exe + .dll under build/<config>/.
# MULTICONFIG: whether the generator lays binaries out under a per-config
# subdir ($BUILD_DIR/Release/) or directly in $BUILD_DIR. That's a property of
# the GENERATOR (Visual Studio / Xcode = multi-config), NOT the platform — a
# Windows build can be either VS (multi) or Ninja (single). Keying off platform
# broke Windows+Ninja, which links to build/bro.exe, not build/Release/bro.exe.
PLATFORM=""
MULTICONFIG=0
if [[ -f "$BUILD_DIR/CMakeCache.txt" ]]; then
    CACHED_GEN="$(grep -E '^CMAKE_GENERATOR:INTERNAL=' "$BUILD_DIR/CMakeCache.txt" | cut -d= -f2 || true)"
    case "$CACHED_GEN" in
        "Visual Studio"*|Xcode) MULTICONFIG=1 ;;
    esac
    if [[ "$CACHED_GEN" == "Visual Studio"* ]]; then
        PLATFORM="win" ; EXE=".exe" ; LIB_GLOB="*.dll"
    fi
fi
if [[ -z "$PLATFORM" ]]; then
    case "$(uname -s)" in
        MINGW*|MSYS*|CYGWIN*) PLATFORM="win" ; EXE=".exe" ; LIB_GLOB="*.dll" ;;
        Darwin)               PLATFORM="macos" ; EXE=""     ; LIB_GLOB="*.dylib" ;;
        Linux)                PLATFORM="linux" ; EXE=""     ; LIB_GLOB="*.so*" ;;
        *) echo "unsupported platform: $(uname -s)" >&2; exit 1 ;;
    esac
fi

# The extension a bronze-compiled app module carries on this platform — the
# app.dll / app.so / app.dylib an app directory is built around.
case "$PLATFORM" in
    win)   MODULE_EXT=".dll" ;;
    macos) MODULE_EXT=".dylib" ;;
    *)     MODULE_EXT=".so" ;;
esac

case "$(uname -m)" in
    x86_64|amd64)       ARCH="x64" ;;
    arm64|aarch64)      ARCH="arm64" ;;
    *)                  ARCH="$(uname -m)" ;;
esac

# Output location depends on the generator, not the platform:
#   multi-config (Visual Studio / Xcode): $BUILD_DIR/$CONFIG/<target>.exe
#   single-config (Ninja / Makefiles):    $BUILD_DIR/<target>
bin_path() {
    local target="$1"
    if [[ "$MULTICONFIG" == "1" ]]; then
        echo "$BUILD_DIR/$CONFIG/$target$EXE"
    else
        echo "$BUILD_DIR/$target$EXE"
    fi
}

BRO_EXE="$(bin_path bro)"
BRO_HEADLESS_EXE="$(bin_path bro-headless)"
BRO_SERVER_EXE="$(bin_path bro-server)"

if [[ ! -x "$BRO_EXE" ]]; then
    echo "error: $BRO_EXE not found. Build first:" >&2
    echo "  cmake --build $BUILD_DIR --config $CONFIG" >&2
    exit 1
fi

OUT_NAME="bro-${VERSION}-${PLATFORM}-${ARCH}"
OUT_DIR="dist/$OUT_NAME"

echo ">>> Packaging $OUT_NAME"
rm -rf "$OUT_DIR"
mkdir -p "$OUT_DIR"

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

# --- Strip debug symbols from our own executables (Linux/macOS) ------------
# Release ELF/Mach-O keep full symbol tables, and with the whole AI tower +
# Skia statically linked in that roughly doubles the binaries (Linux was ~108 MB
# vs Windows' 38 MB, where the PDB already stays out of the zip). Strip only our
# three executables, not the third-party shared libs. macOS uses -x (drop local
# symbols, keep external) so dylib/codesigning stays intact.
if [[ "$PLATFORM" != "win" ]]; then
    STRIP_FLAGS=()
    [[ "$PLATFORM" == "macos" ]] && STRIP_FLAGS=(-x)
    for t in bro bro-headless bro-server; do
        if [[ -f "$OUT_DIR/$t" ]]; then
            strip "${STRIP_FLAGS[@]}" "$OUT_DIR/$t" 2>/dev/null \
                || echo "warning: strip failed for $t, shipping unstripped" >&2
        fi
    done
fi

# --- README + LICENSE -----------------------------------------------------
# Double-clicking bro with no app argument falls through to the built-in
# project manager at system/projects/, so no root bro.json is needed.
cp LICENSE "$OUT_DIR/"
if [[ "$PLATFORM" == "macos" ]]; then
    cat > "$OUT_DIR/README.txt" <<EOF
Bro ${VERSION} (${PLATFORM}-${ARCH})

Run:
  open Bro.app                              # opens the project manager
  Bro.app/Contents/MacOS/bro path/to/app    # runs a specific app from terminal
  ./bro-headless path/to/app script.js      # CLI, headless mode

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
  ./bro${EXE}                    # opens the project manager
  ./bro${EXE} path/to/app        # runs a specific app
  ./bro-headless${EXE} path/to/app script.js
EOF
fi

# --- system/ (global UI: splash, menu, nav, perf, settings, project manager,
#     skeletons) ----------------------------------------------------------
# Resolved by the engine as a relative path from the exe directory.
# system/projects/ is the no-args fallback shown on naked bro.exe; it lists
# user projects and creates new ones from system/skeletons/<name>/.
cp -a system "$OUT_DIR/"

# --- bronze toolchain (only when this build has one) -----------------------
# A build configured -DBRO_WITH_BRONZE=ON can also COMPILE apps, not only run
# them, and shipping bro without that compiler would ship half of the feature:
# the binaries would load an app.dll nobody in the package can produce. So the
# compiler and the libraries it links into what it emits ride along under
# bronze/.
#
# Why a subdirectory and not flat beside bro: both the compiler's own searches
# resolve from ITS exe directory (link.cpp's findRuntimeLib / findSharedRuntime
# look at exeDir first), so bronze/ is a complete, self-contained install of
# it — while the bro root stays the three binaries and system/. The one file
# that must exist in BOTH places is the shared runtime: bro LOADS it at
# startup, so it sits beside bro; the linker RESOLVES it when building a
# module, so a copy sits beside bronze. Two copies of ~1.5 MB, and neither
# search has to know about the other's directory.
#
# The whole block is skipped, silently, when the build has no bronze — that is
# the default configuration and the package is exactly what it always was.
BRONZE_EXE="$(bin_path bronze)"
if [[ -x "$BRONZE_EXE" ]]; then
    BZ_DIR="$OUT_DIR/bronze"
    echo ">>> Staging the bronze compiler into bronze/"
    mkdir -p "$BZ_DIR/include/embed" "$BZ_DIR/include/runtime"
    cp "$BRONZE_EXE" "$BZ_DIR/"

    # Static runtime archives. bro only sets CMAKE_RUNTIME_OUTPUT_DIRECTORY, so
    # unlike the executables these stay in their per-target build directories,
    # with the per-config subdir under a multi-config generator.
    bronze_lib() {
        local mod="$1" base="$BUILD_DIR/bronze-build/src/$1" f
        for f in "$base/$CONFIG/bronze_$mod.lib" "$base/bronze_$mod.lib" \
                 "$base/$CONFIG/libbronze_$mod.a" "$base/libbronze_$mod.a"; do
            [[ -f "$f" ]] && { echo "$f"; return 0; }
        done
        return 1
    }
    for mod in rt runtime json regex; do
        if src="$(bronze_lib "$mod")"; then
            cp "$src" "$BZ_DIR/"
        else
            echo "warning: bronze_$mod library not found under $BUILD_DIR/bronze-build/src/$mod, skipping" >&2
        fi
    done

    # The shared runtime, from bronze's own output directory (CMAKE_BINARY_DIR/
    # shared, per cmake/bronze_shared_runtime.cmake). On Windows what a module
    # LINKS against is the import library, so both it and the DLL come across;
    # elsewhere the shared object is both.
    SHARED_SRC="$BUILD_DIR/shared"
    [[ -d "$SHARED_SRC/$CONFIG" ]] && SHARED_SRC="$SHARED_SRC/$CONFIG"
    # Named, not globbed: a `bronze_runtime_shared.*` glob also drags in the
    # linker's .exp file, which is build scrap. The names that do not exist on
    # this platform are simply skipped.
    for name in bronze_runtime_shared.dll bronze_runtime_shared.lib \
                libbronze_runtime_shared.dylib; do
        if [[ -f "$SHARED_SRC/$name" ]]; then
            cp -a "$SHARED_SRC/$name" "$BZ_DIR/"
        fi
    done
    # ELF is the exception: the soname chain (libfoo.so -> libfoo.so.1) is a set
    # of names, so this one is a glob.
    shopt -s nullglob
    for f in "$SHARED_SRC"/libbronze_runtime_shared.so*; do
        cp -a "$f" "$BZ_DIR/"
    done
    shopt -u nullglob

    # The host-globals manifest: `bronze build` needs it to know which globals
    # bro supplies, so an app compiled for bro cannot be built without this
    # file. It is bro's, not bronze's — it describes THIS host.
    cp src/bronze_host/web_host.globals "$BZ_DIR/"

    # The embed API, for a C++ host of its own that loads compiled modules
    # (embed.h pulls exactly one header, runtime/value.h, so the pair is the
    # whole surface), plus bronze's license alongside bro's.
    BRONZE_SRC="$(grep -E '^BRONZE_DIR:PATH=' "$BUILD_DIR/CMakeCache.txt" | cut -d= -f2- || true)"
    if [[ -n "$BRONZE_SRC" && -d "$BRONZE_SRC" ]]; then
        cp "$BRONZE_SRC/src/embed/embed.h"    "$BZ_DIR/include/embed/"
        cp "$BRONZE_SRC/src/runtime/value.h"  "$BZ_DIR/include/runtime/"
        [[ -f "$BRONZE_SRC/LICENSE" ]] && cp "$BRONZE_SRC/LICENSE" "$BZ_DIR/LICENSE"
    else
        echo "warning: BRONZE_DIR not in $BUILD_DIR/CMakeCache.txt; embed headers not staged" >&2
    fi

    # And say so in the package's own README — a directory nobody is told
    # about is a directory nobody uses.
    cat >> "$OUT_DIR/README.txt" <<EOF

Compiled apps:
  bronze/ is the AOT JavaScript compiler. An app directory carrying an
  app${MODULE_EXT} beside its index.html runs on the binaries above with no
  flag and no separate runtime; bronze/README.txt has the command that
  produces one.
EOF

    cat > "$BZ_DIR/README.txt" <<BZEOF
bronze — the AOT JavaScript compiler for bro ${VERSION}

Compile an app's JavaScript to a native module the stock bro binaries load:

  bronze${EXE} build app.js -o myapp/app${MODULE_EXT} --emit-shared --host-globals web_host.globals

  bro${EXE} myapp

The app directory is index.html + app${MODULE_EXT}; nothing else has to change.
web_host.globals beside this file is the manifest of the globals bro supplies
(DOM, WebGL2, audio, physics, AI); pass it on every build for bro.

bronze also compiles standalone programs — 'bronze${EXE} build prog.js -o prog${EXE}'
links the static runtime archives here into a native executable.

A system linker is required and is not shipped here: MSVC's link.exe on
Windows, clang++ or g++ on Linux and macOS.
BZEOF
fi

# --- macOS .app bundle ----------------------------------------------------
# Finder launches a Mach-O binary in Terminal; wrap everything in a bundle so
# double-click opens the app with no terminal window. Binary + apps + system
# all live under Contents/MacOS so main.cpp's chdir(exeDir()) picks them up.
if [[ "$PLATFORM" == "macos" ]]; then
    APP="$OUT_DIR/Bro.app"
    rm -rf "$APP"
    mkdir -p "$APP/Contents/MacOS"
    # Move the GUI binary and its runtime data into the bundle. Keep the CLI
    # tools (bro-headless, bro-server, and bronze/ when it is here) alongside
    # Bro.app so users can invoke them from a terminal without reaching into
    # the bundle.
    CLI_KEEP=(bro-headless bro-server bronze README.txt LICENSE)
    for item in "$OUT_DIR"/*; do
        name="$(basename "$item")"
        [[ "$name" == "Bro.app" ]] && continue
        keep=0
        for k in "${CLI_KEEP[@]}"; do [[ "$name" == "$k" ]] && keep=1 && break; done
        [[ $keep -eq 1 ]] && continue
        mv "$item" "$APP/Contents/MacOS/"
    done
    # bronze's shared runtime is the one library that has to exist on BOTH
    # sides of the bundle wall: bro loads it from inside Contents/MacOS, and
    # bro-headless — which deliberately stays outside so it can be run from a
    # terminal — resolves it via @loader_path, i.e. beside itself. The move
    # above put it in the bundle, so put a copy back.
    if [[ -f "$APP/Contents/MacOS/libbronze_runtime_shared.dylib" ]]; then
        cp -a "$APP/Contents/MacOS/libbronze_runtime_shared.dylib" "$OUT_DIR/"
    fi
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
elif command -v zip >/dev/null 2>&1; then
    (cd dist && zip -rq "${OUT_NAME}.zip" "$OUT_NAME")
elif command -v powershell.exe >/dev/null 2>&1; then
    # Git-Bash on Windows typically has no `zip`; fall back to PowerShell.
    powershell.exe -NoProfile -Command \
        "Compress-Archive -Path 'dist/$OUT_NAME' -DestinationPath 'dist/${OUT_NAME}.zip' -Force"
else
    echo "error: no zip or powershell available to archive" >&2
    exit 1
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
    echo "  $OUT_DIR/bro-headless path/to/app script.js   # CLI tools stay outside the bundle"
else
    echo "  (cd $OUT_DIR && ./bro$EXE)"
fi
echo ""
echo "Upload $ARCHIVE to your GitHub release."
