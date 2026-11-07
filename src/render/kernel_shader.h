#pragma once

// The render module's own GPU stages (the presenter's compositing, the frame
// diff, a 'kernel' canvas's present) are brokernel (shaders/*.bk), compiled in process by brass to SPIR-V
// the first time each is asked for, and kept.

#include <cstdint>
#include <string>
#include <vector>

namespace bro::render {

struct KernelShader {
    std::vector<uint32_t> spirv;
    std::string entry;   // its OpEntryPoint
    explicit operator bool() const { return !spirv.empty(); }
};

/// The built-in sources, by file name.
enum class RenderKernelSource { Present, FrameDiff, KernelCanvas };

/// `entry` of the built-in source compiled to SPIR-V. Empty, with the
/// diagnostics in `*err` when given, if it does not compile. Thread-safe;
/// each (source, entry) compiles once.
KernelShader renderKernelShader(RenderKernelSource source, const char* entry, std::string* err = nullptr);

/// Every brokernel compile of the process (bro.kernel.compile, the scene's
/// hooks and stages, these) goes through brokernel's on-disk compile cache:
/// `<user cache dir>/kernel-cache`, held to 512 MB; BRO_KERNEL_CACHE=0 turns
/// it off, BRO_KERNEL_CACHE_DIR moves it (docs/code-cache.md). Called once at
/// launch, before the first compile; idempotent.
void configureKernelCache();
/// Waits for a trim of the cache in progress (engine shutdown).
void flushKernelCache();

} // namespace bro::render
