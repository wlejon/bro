#pragma once

#include "scene/scene_renderer.h"
#include <vulkan/vulkan.h>
#include <string>
#include <vector>

namespace bro::scene::vk {

class SceneVkCustomShader {
public:
    /// Preprocess user GLSL chunk by grouping non-opaque uniforms into
    /// layout(set = 4, binding = 0, std140) uniform UserUniforms { ... };
    /// and assigning samplers to layout(set = 4, binding = 1..N).
    static std::string preprocessUserGlsl(const std::string& chunk,
                                          uint32_t setIndex,
                                          std::vector<std::string>& outSamplerNames);

    /// Compute std140 layout offsets for user uniforms declared across chunks.
    static std::vector<std::pair<std::string, uint32_t>> parseUniformOffsets(const std::string& chunk,
                                                                            uint32_t& outTotalSize);

    /// Validate GLSL syntax and hooks by compiling it.
    static bool validateCustomShader(SceneRenderer::CustomShaderTarget target,
                                     const std::string& vertexChunk,
                                     const std::string& fragmentChunk,
                                     std::string& errOut);

    /// Compile vertex and fragment shader stages with spliced user chunks.
    static bool compileCustomShaderModules(VkDevice device,
                                           SceneRenderer::CustomShaderTarget target,
                                           const std::string& vertexChunk,
                                           const std::string& fragmentChunk,
                                           VkShaderModule& outVs,
                                           VkShaderModule& outFs,
                                           std::vector<std::string>& outSamplerNames,
                                           std::string& errOut);

    /// Compile shadow vertex shader stage with spliced user vertex chunk.
    static bool compileCustomShadowShaderModule(VkDevice device,
                                                bool isSkinned,
                                                const std::string& vertexChunk,
                                                VkShaderModule& outVs,
                                                std::string& errOut);
};

} // namespace bro::scene::vk
