#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace bro::render {

enum class ShaderStage { Vertex, Fragment, Compute };

/// Compile Vulkan-flavoured GLSL (`#version 450` with explicit layouts) to
/// SPIR-V 1.0 for a Vulkan 1.0 client, in process, with glslang — the one
/// shader compiler bro has. The scene's built-in shaders go through it at
/// build time (bro_spirv_embed), and the clipmap terrain, custom scene shaders
/// and WebGL programs at run time.
///
/// Returns the SPIR-V words, or an empty vector with glslang's diagnostics in
/// `*log` (when given) if the source does not compile. Successful results are
/// memoised by (stage, source), since WebGL compiles each shader once at
/// compileShader and again at link. Thread-safe.
std::vector<uint32_t> compileGlslToSpirv(std::string_view source, ShaderStage stage,
                                         std::string* log = nullptr);

} // namespace bro::render
