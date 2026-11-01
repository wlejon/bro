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

} // namespace bro::render
