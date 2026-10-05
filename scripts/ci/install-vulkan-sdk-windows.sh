#!/usr/bin/env bash
# Install the LunarG Vulkan SDK on a GitHub Actions Windows runner: the Vulkan
# headers and vulkan-1.lib bro links (the loader DLL itself ships with every
# GPU driver, so the package never carries it). Pinned by version and SHA-256
# (VULKAN_SDK_VERSION / VULKAN_SDK_SHA256, set by the workflow), so a CI
# configure is reproducible. Exports VULKAN_SDK, which CMake's FindVulkan reads,
# and puts the SDK's Bin/ on PATH.
#
# The runner has no GPU and no Vulkan driver, so this is what lets bro
# CONFIGURE and BUILD there; it does not make bro's GPU path runnable.
set -euo pipefail

VER="${VULKAN_SDK_VERSION:?VULKAN_SDK_VERSION is not set}"
SHA="${VULKAN_SDK_SHA256:?VULKAN_SDK_SHA256 is not set}"
EXE="$RUNNER_TEMP/vulkansdk-$VER.exe"
ROOT="C:/VulkanSDK/$VER"

curl -fsSL --retry 3 -o "$EXE" \
    "https://sdk.lunarg.com/sdk/download/$VER/windows/vulkansdk-windows-X64-$VER.exe"
echo "$SHA  $EXE" | sha256sum -c -
"$EXE" --root "$ROOT" --accept-licenses --default-answer --confirm-command install
rm -f "$EXE"

[[ -f "$ROOT/Include/vulkan/vulkan.h" && -f "$ROOT/Lib/vulkan-1.lib" ]] || {
    echo "error: Vulkan SDK install at $ROOT is missing headers or vulkan-1.lib" >&2
    exit 1
}
echo "VULKAN_SDK=$ROOT" >> "$GITHUB_ENV"
echo "$ROOT/Bin" >> "$GITHUB_PATH"
