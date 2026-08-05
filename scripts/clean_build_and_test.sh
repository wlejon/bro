#!/usr/bin/env bash
# Clean-build-and-test gate for bro stack.
# Wipes build artifacts, performs a fresh CMake configure & build,
# runs the integration test suite, and reports verifiable test counts.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"

cd "$PROJECT_DIR"

echo "================================================================"
echo "  CLEAN BUILD AND TEST GATE"
echo "  Project: $PROJECT_DIR"
echo "================================================================"

echo "[1/3] Cleaning build directory..."
rm -rf "$PROJECT_DIR/build"

echo "[2/3] Configuring & Building (Release)..."
if [[ "${OSTYPE:-}" == "msys" || "${OSTYPE:-}" == "cygwin" || -n "${WINDIR:-}" ]]; then
    cmake -B build -DCMAKE_BUILD_TYPE=Release "$@"
    cmake --build build --config Release --parallel 4
else
    cmake -B build -DCMAKE_BUILD_TYPE=Release "$@"
    cmake --build build --config Release --parallel 4
fi

echo "[3/3] Executing test suite..."
bash "$PROJECT_DIR/tests/run_tests.sh"
