#pragma once

#include <cstdint>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

namespace bro::render {

enum class ShaderStage { Vertex, Fragment, Compute };

/// Compile Vulkan-flavoured GLSL (`#version 450` with explicit layouts) to
/// SPIR-V 1.0 for a Vulkan 1.0 client, in process, with glslang — the one
/// shader compiler bro has. The scene's built-in shaders go through it at
/// build time (bro_spirv_embed), and the clipmap terrain and custom scene
/// shaders at run time; WebGL programs use glslang directly (acquireGlslang)
/// because linking them needs its reflection.
///
/// Returns the SPIR-V words, or an empty vector with glslang's diagnostics in
/// `*log` (when given) if the source does not compile. Successful results are
/// memoised by (stage, source), since the same variant is often requested
/// again (a material rebuilt, a terrain recreated). Thread-safe.
std::vector<uint32_t> compileGlslToSpirv(std::string_view source, ShaderStage stage,
                                         std::string* log = nullptr);

/// For code that drives glslang's C++ API itself (WebGL's program linker
/// needs its reflection): the lock compileGlslToSpirv holds, taken with the
/// process initialised. glslang may be used for as long as the returned lock
/// is held; the lock is empty (owns_lock() false) if initialisation failed.
std::unique_lock<std::mutex> acquireGlslang();

} // namespace bro::render
