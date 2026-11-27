#!/usr/bin/env bash
# Two-stage PGO driver for bro's bronze-compiled built-in JS modules
# (src/bronze_host/CMakeLists.txt: bro_bronze_js).
#
# Stage 1 compiles bro_bronze_js with -DBRO_JS_HARVEST_DIR=<out_dir> so each
# built-in module records inline-cache & counter evidence, then runs training
# workloads through bro-headless so writeInlineCacheHarvests() flushes exact
# .tfb profiles (matching each module's icLayoutDigest).
# Stage 2 reconfigures with -DBRO_JS_HARVEST_DIR="" -DBRO_JS_PROFILE_DIR=<out_dir>
# and rebuilds bro_bronze_js with --feedback-in.
#
# Pass --smoke-only to verify --emit-obj --harvest-out + --feedback-in directly
# with the bronze compiler on geometry.js without reconfiguring the full build.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD_DIR="${BUILD_DIR:-$ROOT/build}"
OUT_DIR="${OUT_DIR:-$BUILD_DIR/js-profiles}"
SMOKE_ONLY=0

for arg in "$@"; do
    case "$arg" in
        --smoke-only) SMOKE_ONLY=1 ;;
        --help|-h)
            echo "usage: $0 [--smoke-only] [out_dir]"
            exit 0
            ;;
        *) OUT_DIR="$arg" ;;
    esac
done

mkdir -p "$OUT_DIR"

if [[ "$SMOKE_ONLY" == "1" ]]; then
    BRONZE="${BRONZE:-}"
    for cand in "$BUILD_DIR/bronze" "$ROOT/../bronze/build/bronze"; do
        if [[ -x "$cand" ]]; then BRONZE="$cand"; break; fi
    done
    if [[ -z "$BRONZE" || ! -x "$BRONZE" ]]; then
        echo "harvest-builtin-js: bronze binary not found" >&2
        exit 1
    fi
    TMP_OBJ="$(mktemp /tmp/bro_geom_XXXXXX.o)"
    trap 'rm -f "$TMP_OBJ"' EXIT
    "$BRONZE" build "$ROOT/src/bronze_host/js/geometry.js" \
        --emit-obj \
        --host-globals "$ROOT/src/bronze_host/js/module.globals" \
        --harvest-out "$OUT_DIR/geometry.tfb" \
        --entry-symbol=bro_geometry_main \
        -o "$TMP_OBJ"
    echo "harvest-builtin-js --smoke-only: emitted object with --harvest-out -> $OUT_DIR/geometry.tfb"
    exit 0
fi

echo "==> Stage 1: building bro-headless with BRO_JS_HARVEST_DIR=$OUT_DIR"
cmake -S "$ROOT" -B "$BUILD_DIR" -DBRO_JS_HARVEST_DIR="$OUT_DIR" -DBRO_JS_PROFILE_DIR=""
cmake --build "$BUILD_DIR" --target bro-headless -j 4

echo "==> Training built-in JS modules to harvest .tfb profiles"
"$ROOT/tests/run_tests.sh" dom scene net workers || true

echo "==> Stage 2: rebuilding bro_bronze_js with BRO_JS_PROFILE_DIR=$OUT_DIR"
cmake -S "$ROOT" -B "$BUILD_DIR" -DBRO_JS_HARVEST_DIR="" -DBRO_JS_PROFILE_DIR="$OUT_DIR"
cmake --build "$BUILD_DIR" --target bro_bronze_js -j 4
echo "Done: harvested profiles in $OUT_DIR"
