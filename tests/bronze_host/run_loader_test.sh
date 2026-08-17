#!/usr/bin/env bash
# Integration check for the app-module LOADER (src/bronze_host/app_module.h):
# the stock bro-headless opening an app directory that carries a compiled
# module, and refusing the ones it must refuse.
#
# WHY THIS CHECK IS DIFFERENT FROM ITS NEIGHBOURS. Every other script here
# drives a bro-bronze-host-<app> binary with an app LINKED IN, so what they
# test is the host layer. This one tests the stock binary — the same
# bro-headless every other test in tests/ uses — because the whole point of
# the folder model is that ONE binary opens an interpreted app and a compiled
# one, and which it got is a property of the directory.
#
# WHY THE MODULES ARE SYNTHETIC. The subject is the loader's contract, not
# bronze's codegen: what a module must export, and what happens when it does
# not. Four ~3-line C files cover every branch exactly, deterministically, and
# without a bronze CLI in the loop — a real compiled app could only ever
# exercise the success path, and the failure paths are the ones that matter.
# A module whose ABI stamp disagrees is the failure this whole mechanism
# exists for (bronze_abi.h's "Drift between two BUILDS"): a stale module loads
# happily where a stale linked object at least forced a relink, and the
# symptom is nondeterministic stalls rather than a crash. So `wrongabi`'s
# bronze_main prints a line that must NOT appear, and the check asserts its
# absence — proving the guard prevents execution rather than reporting it
# afterwards.
#
# WHY THE FINGERPRINT IS RECOMPUTED HERE. It is the first 32 bits of
# bronze_abi.h's SHA-256 (bronze's src/abi/CMakeLists.txt). Deriving it the
# same way bronze's build does, from the same file, is what lets `good.dll`
# match and `wrongabi.dll` differ without either value being written down
# anywhere it could go stale.
#
# Exit 0 pass, 1 fail, 77 skip — as its neighbours.
#
# Usage:
#   tests/bronze_host/run_loader_test.sh
#
# Environment:
#   BRO_HEADLESS   path to bro-headless
#   BRONZE_DIR     bronze checkout (default: read from the build cache, else ../bronze)

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/../.." && pwd)"
NAME="bronze_host_loader"

skip() { echo "  SKIP  $NAME  ($1)"; exit 77; }
fail() { echo "  FAIL  $NAME  ($1)"; shift; [[ $# -gt 0 ]] && printf '%s\n' "$@" | sed 's/^/        /'; exit 1; }

# --- The binary under test: the STOCK headless driver ----------------------
BIN=""
if [[ -n "${BRO_HEADLESS:-}" ]]; then
    BIN="$BRO_HEADLESS"
else
    for CANDIDATE in \
        "$PROJECT_DIR/build/Release/bro-headless.exe" \
        "$PROJECT_DIR/build/Debug/bro-headless.exe" \
        "$PROJECT_DIR/build/bro-headless" \
        "$PROJECT_DIR/build-release/bro-headless" \
        "$PROJECT_DIR/build-debug/bro-headless"
    do
        [[ -f "$CANDIDATE" ]] && BIN="$CANDIDATE" && break
    done
fi
[[ -z "$BIN" || ! -f "$BIN" ]] && skip "bro-headless not built"

# Everything below is a question about the build THIS BINARY came from, so find
# that build's cache rather than any cache in the tree. A checkout can hold a
# dozen build directories pinned to different bronze checkouts, and reading the
# wrong one would compute a fingerprint the binary under test does not speak —
# which looks exactly like the ABI-mismatch failure this check is here to
# detect. Windows multi-config puts the exe one level deeper than Ninja does.
BUILD_DIR="$(cd "$(dirname "$BIN")" && pwd)"
[[ -f "$BUILD_DIR/CMakeCache.txt" ]] || BUILD_DIR="$(dirname "$BUILD_DIR")"
CACHE="$BUILD_DIR/CMakeCache.txt"
[[ -f "$CACHE" ]] || skip "cannot find the CMake cache for $BIN"

# The loader only exists in a build configured with BRO_WITH_BRONZE=ON; in any
# other build the app dir's module is simply never looked for, and every case
# below would "fail" by reporting nothing at all. Distinguish the two.
grep -q "^BRO_WITH_BRONZE:BOOL=ON" "$CACHE" \
    || skip "$BUILD_DIR was not configured with -DBRO_WITH_BRONZE=ON"

command -v cmake >/dev/null 2>&1 || skip "cmake not on PATH (needed to build the synthetic modules)"

# --- Where bronze is, and therefore what ABI this bro speaks ----------------
if [[ -z "${BRONZE_DIR:-}" ]]; then
    BRONZE_DIR="$(grep "^BRONZE_DIR:PATH=" "$CACHE" | head -1 | cut -d= -f2-)"
fi
[[ -z "${BRONZE_DIR:-}" ]] && BRONZE_DIR="$PROJECT_DIR/../bronze"
ABI_HEADER="$BRONZE_DIR/src/abi/bronze_abi.h"
[[ -f "$ABI_HEADER" ]] || skip "cannot find bronze_abi.h under $BRONZE_DIR"
FP="$(sha256sum "$ABI_HEADER" | cut -c1-8)"

case "$(uname -s)" in
    Darwin) EXT=".dylib" ;;
    MINGW*|MSYS*|CYGWIN*) EXT=".dll" ;;
    *) EXT=".so" ;;
esac

to_win_path() {
    local p="$1"
    if [[ "$BIN" == *.exe ]]; then
        if [[ "$p" =~ ^/mnt/([a-zA-Z])/(.*) ]]; then echo "${BASH_REMATCH[1]}:/${BASH_REMATCH[2]}"
        elif [[ "$p" =~ ^/([a-zA-Z])/(.*) ]]; then echo "${BASH_REMATCH[1]}:/${BASH_REMATCH[2]}"
        else echo "$p"; fi
    else
        echo "$p"
    fi
}

WORK="$(mktemp -d 2>/dev/null || echo "${TMPDIR:-/tmp}/bro_loader_$$")"
mkdir -p "$WORK"
trap 'rm -rf "$WORK"' EXIT

# --- Four modules, one per branch of runAppModule --------------------------
SRC="$WORK/src"; mkdir -p "$SRC"
cat > "$SRC/export.h" <<'EOF'
#ifdef _WIN32
#define MOD_EXPORT __declspec(dllexport)
#else
#define MOD_EXPORT __attribute__((visibility("default")))
#endif
EOF
# No ABI stamp at all: a DLL that is simply not a bronze module.
cat > "$SRC/unstamped.c" <<'EOF'
#include "export.h"
MOD_EXPORT int not_a_bronze_module(void) { return 1; }
EOF
# Stamped with an ABI this runtime does not speak. Its bronze_main prints a
# line the check asserts is ABSENT: the guard must prevent the call.
cat > "$SRC/wrongabi.c" <<EOF
#include "export.h"
#include <stdio.h>
MOD_EXPORT const unsigned int bronze_object_abi_fingerprint = 0xDEADBEEFu;
MOD_EXPORT void bronze_main(void) { printf("LOADER: stale module ran\n"); fflush(stdout); }
EOF
# Right ABI, but compiled as a library rather than an app entry point.
cat > "$SRC/noentry.c" <<EOF
#include "export.h"
MOD_EXPORT const unsigned int bronze_object_abi_fingerprint = 0x${FP}u;
EOF
# The success path.
cat > "$SRC/good.c" <<EOF
#include "export.h"
#include <stdio.h>
MOD_EXPORT const unsigned int bronze_object_abi_fingerprint = 0x${FP}u;
MOD_EXPORT void bronze_main(void) { printf("LOADER: bronze_main called\n"); fflush(stdout); }
EOF
cat > "$SRC/CMakeLists.txt" <<'EOF'
cmake_minimum_required(VERSION 3.20)
project(bro_loader_fixtures C)
foreach(m unstamped wrongabi noentry good)
    add_library(${m} SHARED ${m}.c)
    set_target_properties(${m} PROPERTIES
        PREFIX "" C_VISIBILITY_PRESET hidden
        LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/out"
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/out"
        LIBRARY_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}/out"
        RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}/out")
endforeach()
EOF
if ! cmake -S "$SRC" -B "$WORK/b" > "$WORK/cmake.log" 2>&1 \
   || ! cmake --build "$WORK/b" --config Release >> "$WORK/cmake.log" 2>&1; then
    skip "could not build the synthetic modules (see $WORK/cmake.log)"
fi
OUT="$WORK/b/out"
[[ -f "$OUT/good$EXT" ]] || skip "synthetic modules did not land in $OUT"

# --- The app directory each case is run against ----------------------------
APP="$WORK/app"; mkdir -p "$APP"
cat > "$APP/bro.json" <<'EOF'
{ "title": "loader check", "compiled": true, "width": 320, "height": 240, "splash": false }
EOF
cat > "$APP/index.html" <<'EOF'
<!doctype html><html><body></body></html>
EOF
APP_ARG="$(to_win_path "$APP")"

run_case() {
    rm -f "$APP/app$EXT"
    [[ -n "$2" ]] && cp "$2" "$APP/app$EXT"
    "$BIN" "$APP_ARG" -e "1" 2>&1 | tr -d '\r'
}

check() {  # name, module-path-or-empty, must-contain, [must-not-contain]
    local label="$1" module="$2" want="$3" reject="${4:-}"
    local out; out="$(run_case "$label" "$module")"
    if ! printf '%s\n' "$out" | grep -qF "$want"; then
        fail "$label: expected output containing '$want'" "$(printf '%s\n' "$out" | tail -12)"
    fi
    if [[ -n "$reject" ]] && printf '%s\n' "$out" | grep -qF "$reject"; then
        fail "$label: output contained '$reject', which must never appear" \
             "$(printf '%s\n' "$out" | tail -12)"
    fi
}

# An app dir that declares "compiled": true and carries nothing: the engine's
# own diagnostic, not the loader's — the loader is never reached.
check "no module" "" 'carries no compiled module'

# A file that is not a loadable image at all.
printf 'this is not a shared library' > "$WORK/garbage$EXT"
check "garbage module" "$WORK/garbage$EXT" 'could not be loaded'

check "unstamped module" "$OUT/unstamped$EXT" 'is not a bronze-compiled app'

# The one that matters: refused, both fingerprints named, and NOT executed.
check "stale ABI" "$OUT/wrongabi$EXT" \
      "was compiled against bronze ABI deadbeef, but this bro speaks $FP" \
      'LOADER: stale module ran'

check "no entry point" "$OUT/noentry$EXT" 'exports no bronze_main'

# Success: host globals installed, the compiled top level called.
check "loads and runs" "$OUT/good$EXT" 'LOADER: bronze_main called'
# Trailing comma, not a closing paren: the success line goes on to name the
# host-globals manifest the module declared, and pinning the whole line would
# make this check fail every time that list is worded differently.
check "success is logged" "$OUT/good$EXT" "(bronze ABI $FP,"

echo "  PASS  $NAME"
exit 0
