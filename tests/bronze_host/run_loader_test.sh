#!/usr/bin/env bash
# Integration check for the app-module LOADER (src/bronze_host/app_module.h):
# the stock bro-headless opening an app directory that carries a compiled
# module, and refusing the ones it must refuse.
#
# WHY THIS CHECK IS DIFFERENT FROM ITS NEIGHBOURS. Every other script here
# runs a real compiled app to test the HOST LAYER beneath it. This one tests
# the LOADER itself, on the same stock bro-headless every other test in tests/
# uses, because the whole point of the folder model is that ONE binary opens an
# interpreted app and a compiled one, and which it got is a property of the
# directory.
#
# WHY THE MODULES ARE SYNTHETIC. The subject is the loader's contract, not
# bronze's codegen: what a module must export, and what happens when it does
# not. Four ~3-line C files cover every branch exactly, deterministically, and
# without a bronze CLI in the loop ? a real compiled app could only ever
# exercise the success path, and the failure paths are the ones that matter.
# A module whose ABI stamp disagrees is the failure this whole mechanism
# exists for (bronze_abi.h's "Drift between two BUILDS"): a stale module loads
# happily where a stale linked object at least forced a relink, and the
# symptom is nondeterministic stalls rather than a crash. So `wrongabi`'s
# bronze_main prints a line that must NOT appear, and the check asserts its
# absence ? proving the guard prevents execution rather than reporting it
# afterwards.
#
# WHY THE FINGERPRINT IS RECOMPUTED HERE. It is the first 32 bits of the
# SHA-256 of bronze_abi.h followed by bronze_abi_tls.h, when the latter exists
# (bronze's src/abi/CMakeLists.txt). Deriving it the same way bronze's build
# does, from the same files, is what lets `good.dll`
# match and `wrongabi.dll` differ without either value being written down
# anywhere it could go stale.
#
# Exit 0 pass, 1 fail, 77 skip ? as its neighbours.
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
# wrong one would compute a fingerprint the binary under test does not speak ?
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
BRONZE_DIR="${BRONZE_DIR//$'\r'/}"
[[ -z "${BRONZE_DIR:-}" ]] && BRONZE_DIR="$PROJECT_DIR/../bronze"
if [[ "$BRONZE_DIR" =~ ^([a-zA-Z]):[/\\](.*) ]]; then
    letter="${BASH_REMATCH[1]}"
    letter_lower="$(echo "$letter" | tr '[:upper:]' '[:lower:]')"
    rest="${BASH_REMATCH[2]}"
    if [[ -d "/mnt/$letter_lower" ]]; then
        BRONZE_DIR="/mnt/$letter_lower/$rest"
    elif [[ -d "/$letter_lower" ]]; then
        BRONZE_DIR="/$letter_lower/$rest"
    fi
fi
ABI_HEADER="$BRONZE_DIR/src/abi/bronze_abi.h"
[[ -f "$ABI_HEADER" ]] || skip "cannot find bronze_abi.h under $BRONZE_DIR"
ABI_TLS_HEADER="$BRONZE_DIR/src/abi/bronze_abi_tls.h"
# sha256sum is coreutils; macOS before 14 has only shasum.
sha256() { if command -v sha256sum >/dev/null 2>&1; then sha256sum "$@"; else shasum -a 256 "$@"; fi; }
# bronze_abi.h's text is kept in parts (bronze's cmake/bronze_abi_text.cmake):
# a part named there is put back in place of its `#include "abi/<part>"` line
# before hashing, exactly as bronze's build reads it. The TLS header keeps its
# include and is hashed after, on its own.
abi_text() {
    awk -v dir="$(dirname "$ABI_HEADER")" '
        /^#include "abi\/bronze_abi_(functions|layout)\.h"$/ {
            part = $0; sub(/^#include "abi\//, "", part); sub(/"$/, "", part)
            f = dir "/" part
            while ((getline l < f) > 0) print l
            close(f)
            next
        }
        { print }' "$ABI_HEADER"
}
if [[ -f "$ABI_TLS_HEADER" ]]; then
    FP="$( { abi_text; cat "$ABI_TLS_HEADER"; } | sha256 | cut -c1-8)"
else
    FP="$(abi_text | sha256 | cut -c1-8)"
fi

case "$(uname -s)" in
    Darwin) EXT=".dylib" ;;
    MINGW*|MSYS*|CYGWIN*) EXT=".dll" ;;
    *)
        if [[ "$BIN" == *.exe ]]; then EXT=".dll"
        else EXT=".so"; fi
        ;;
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

WORK="$(mktemp -d -p "$BUILD_DIR" 2>/dev/null || mktemp -d 2>/dev/null || echo "$BUILD_DIR/bro_loader_$$")"
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
# Manifest declares host globals that ARE installed on the runtime.
cat > "$SRC/goodglobals.c" <<EOF
#include "export.h"
#include <stdint.h>
#include <stdio.h>
struct BronzeHostGlobals { uint32_t count; char names[128]; };
MOD_EXPORT const unsigned int bronze_object_abi_fingerprint = 0x${FP}u;
MOD_EXPORT const struct BronzeHostGlobals bronze_main_host_globals = {
    2u, "document\0performance\0"
};
MOD_EXPORT void bronze_main(void) { printf("LOADER: good globals module ran\n"); fflush(stdout); }
EOF
# Manifest declares host globals that are NOT on the runtime: must be refused
# before bronze_main is ever called.
cat > "$SRC/missingglobals.c" <<EOF
#include "export.h"
#include <stdint.h>
#include <stdio.h>
struct BronzeHostGlobals { uint32_t count; char names[128]; };
MOD_EXPORT const unsigned int bronze_object_abi_fingerprint = 0x${FP}u;
MOD_EXPORT const struct BronzeHostGlobals bronze_main_host_globals = {
    3u, "document\0__missing_host_global_1\0__missing_host_global_2\0"
};
MOD_EXPORT void bronze_main(void) { printf("LOADER: missing globals module ran\n"); fflush(stdout); }
EOF
cat > "$SRC/CMakeLists.txt" <<'EOF'
cmake_minimum_required(VERSION 3.20)
project(bro_loader_fixtures C)
foreach(m unstamped wrongabi noentry good goodglobals missingglobals)
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
if [[ ! -f "$OUT/good$EXT" && -f "$OUT/Release/good$EXT" ]]; then
    OUT="$OUT/Release"
fi
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
    # stdout (the module's own prints) and stderr (the engine log, written from
    # several threads) are captured apart and printed one after the other: one
    # merged pipe lets a line from one land inside a line of the other, and a
    # searched-for string split that way reads as missing.
    local err="$WORK/run_case.err"
    "$BIN" "$APP_ARG" -e "1" 2>"$err" | tr -d '\r'
    tr -d '\r' < "$err"
    rm -f "$err"
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
# own diagnostic, not the loader's ? the loader is never reached.
check "no module" "" 'host provides no compiled app module'

# A file that is not a loadable image at all.
printf 'this is not a shared library' > "$WORK/garbage$EXT"
check "garbage module" "$WORK/garbage$EXT" 'could not be loaded'

check "unstamped module" "$OUT/unstamped$EXT" 'is not a bronze-compiled app'

# The one that matters: refused, both fingerprints named, and NOT executed.
check "stale ABI" "$OUT/wrongabi$EXT" \
      "was compiled against bronze ABI deadbeef, but this bro speaks $FP" \
      'LOADER: stale module ran'

check "no entry point" "$OUT/noentry$EXT" 'exports no bronze_main'

# Host-globals manifest: missing globals are refused BEFORE bronze_main runs.
check "missing host globals" "$OUT/missingglobals$EXT" \
      'host globals missing from runtime: __missing_host_global_1, __missing_host_global_2' \
      'LOADER: missing globals module ran'

# Host-globals manifest: present globals succeed and run bronze_main.
check "present host globals" "$OUT/goodglobals$EXT" \
      'LOADER: good globals module ran'

# Success: host globals installed, the compiled top level called.
check "loads and runs" "$OUT/good$EXT" 'LOADER: bronze_main called'
# Trailing comma, not a closing paren: the success line goes on to name the
# host-globals manifest the module declared, and pinning the whole line would
# make this check fail every time that list is worded differently.
check "success is logged" "$OUT/good$EXT" "(bronze ABI $FP,"

# --- eBPF-style verified applet gate (app.cert.json + capability manifest) --
GOOD_HASH="$(python3 -c '
import sys
data = open(sys.argv[1], "rb").read()
h = 0xcbf29ce484222325
for b in data:
    h = ((h ^ b) * 0x100000001b3) & 0xffffffffffffffff
print(f"{h:016x}")
' "$OUT/good$EXT")"

cat > "$APP/bro.json" <<'EOF'
{ "title": "loader check", "compiled": true, "verifiedApplet": true, "capabilities": ["bro.ui"], "width": 320, "height": 240, "splash": false }
EOF

# 1. Missing app.cert.json when verifiedApplet: true -> refused before bronze_main runs
rm -f "$APP/app.cert.json"
check "verified applet missing cert" "$OUT/good$EXT" \
      'missing applet verification certificate' \
      'LOADER: bronze_main called'

# 2. Tampered / mismatched moduleHash in app.cert.json -> refused before bronze_main runs
cat > "$APP/app.cert.json" <<'EOF'
{
  "verified": true,
  "hazards": 0,
  "proofsValid": true,
  "moduleHash": "0000000000000000",
  "capabilities": ["bro.ui.setTitle"]
}
EOF
check "verified applet tampered hash" "$OUT/good$EXT" \
      'applet certificate hash mismatch' \
      'LOADER: bronze_main called'

# 3. Unauthorized capability (child_process) in app.cert.json -> refused before bronze_main runs
cat > "$APP/app.cert.json" <<EOF
{
  "verified": true,
  "hazards": 0,
  "proofsValid": true,
  "moduleHash": "$GOOD_HASH",
  "capabilities": ["bro.ui.setTitle", "child_process"]
}
EOF
check "verified applet unauthorized capability" "$OUT/good$EXT" \
      "applet certificate requests unauthorized capability 'child_process'" \
      'LOADER: bronze_main called'

# 4. Valid app.cert.json + permitted capability -> loads and runs bronze_main
cat > "$APP/app.cert.json" <<EOF
{
  "verified": true,
  "hazards": 0,
  "proofsValid": true,
  "moduleHash": "$GOOD_HASH",
  "capabilities": ["bro.ui.setTitle"]
}
EOF
check "verified applet valid cert runs" "$OUT/good$EXT" \
      'LOADER: bronze_main called'

rm -f "$APP/app.cert.json"
cat > "$APP/bro.json" <<'EOF'
{ "title": "loader check", "compiled": true, "width": 320, "height": 240, "splash": false }
EOF

# --- Lexical scanner checks (hasAwaitStmt / hasImportStmt) ------------------
SCAN_APP="$WORK/scan_app"; mkdir -p "$SCAN_APP"
cat > "$SCAN_APP/index.html" <<'EOF'
<!doctype html><html><body></body></html>
EOF

cat > "$SCAN_APP/driver.js" <<'EOF'
// 1. False-positive check: `await` and `import` at line-start inside comments,
// strings, template literals, and nested async functions/arrows must NOT trigger
// wrapAsyncIife (which would turn synchronous eval's completion value into a
// Promise resolving to undefined).
const syncEvalRes = eval([
  "/*",
  "await not_top_level_in_comment",
  "import fake from 'also_in_comment'",
  "*/",
  "const s = 'await in_string\\nimport fake2 from \"in_string\"';",
  "const tpl = `",
  "await not_top_level_in_template",
  "import fake3 from 'in_template'",
  "`;",
  "async function nestedAsync() {",
  "  await Promise.resolve(10);",
  "}",
  "const arrowAsync = async () => {",
  "  await Promise.resolve(20);",
  "};",
  "const obj = { await: 1, import: 2 };",
  "12345"
].join("\n"));
console.log("SCAN:sync-eval=" + typeof syncEvalRes + ":" + syncEvalRes);

// 2. True-positive top-level await & false-positive import check:
// Comments/strings/templates with line-leading `import` must NOT trigger
// hasImportStmt, while mid-line, branch-interior, and `${...}` template
// interpolation top-level `await` MUST trigger hasAwaitStmt.
/*
import broken from "./nonexistent_comment_module.js"
*/
const commentImportStr = "\nimport broken from './nonexistent_str_module.js'\n";
const commentImportTpl = `
import broken from "./nonexistent_tpl_module.js"
`;
const midLine = await Promise.resolve(42);
let branchVal = 0;
if (midLine === 42) {
  branchVal = await Promise.resolve(7);
}
const interp = `sum=${await Promise.resolve(midLine + branchVal)}`;
console.log("SCAN:driver=" + interp);
EOF

SCAN_APP_ARG="$(to_win_path "$SCAN_APP")"
SCAN_DRIVER_ARG="$(to_win_path "$SCAN_APP/driver.js")"
SCAN_ERR="$WORK/scan.err"
SCAN_OUT="$("$BIN" "$SCAN_APP_ARG" "$SCAN_DRIVER_ARG" 2>"$SCAN_ERR" | tr -d '\r'; tr -d '\r' < "$SCAN_ERR")"
rm -f "$SCAN_ERR"

if ! printf '%s\n' "$SCAN_OUT" | grep -qF "SCAN:sync-eval=number:12345"; then
    fail "scanner sync-eval: comments/templates/nested async falsely triggered async IIFE wrapper" \
         "$(printf '%s\n' "$SCAN_OUT" | head -20)"
fi
if ! printf '%s\n' "$SCAN_OUT" | grep -qF "SCAN:driver=sum=49"; then
    fail "scanner driver: mid-line/branch/template-interpolation top-level await or comment import failed" \
         "$(printf '%s\n' "$SCAN_OUT" | head -20)"
fi

echo "  PASS  $NAME"
exit 0
