#!/usr/bin/env bash
# One-shot Linux release build — intended to run inside the linux-build
# container (see docker-compose.yml). Outputs dist/bro-<version>-linux-x64.zip
# on the host.

set -euo pipefail

cd "$(dirname "$0")/.."

VERSION="${RELEASE_VERSION:-}"
if [[ -z "$VERSION" ]]; then
    VERSION="$(git describe --tags --always --dirty 2>/dev/null || echo 0.0.0)"
fi

# Skia — build once; subsequent runs reuse the cached output volume.
if [[ ! -f third_party/skia/lib/Release/libskia.a ]]; then
    echo ">>> Building Skia (first run, ~20 min)"
    bash third_party/skia/build_skia_linux.sh
else
    echo ">>> Skia already built, skipping"
fi

# Configure + build bro in a dedicated Linux build dir.
if [[ ! -f build-linux/CMakeCache.txt ]]; then
    echo ">>> Configuring"
    cmake -B build-linux -DCMAKE_BUILD_TYPE=Release -G Ninja
fi

echo ">>> Building"
cmake --build build-linux -j"$(nproc)"

echo ">>> Packaging"
bash scripts/package-release.sh --version "$VERSION" --build-dir build-linux
