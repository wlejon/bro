#pragma once

#include "webgl/vulkan/webgl_vk_types.h"
#include "webgl/vulkan/webgl_vk_shaders_parser.h"

#include <vulkan/vulkan.h>
#include "webgl/webgl_types.h"
#include <string>
#include <vector>
#include <unordered_map>

namespace bro::webgl::vk {

/// Results of shader translation for Vulkan.
struct TranslatedShader {
    std::string source;
    std::vector<VkUniformInfo> uniforms;
    std::unordered_map<std::string, GLint> attributeLocations;
    std::unordered_map<std::string, GLint> samplerBindings;
    uint32_t pushConstantSize = 0;
};

/// Translates WebGL GLSL to Vulkan GLSL and compiles via glslc to SPIR-V.
class WebGLVkShaderCompiler {
public:
    WebGLVkShaderCompiler() = default;
    ~WebGLVkShaderCompiler() = default;

    /// Translate WebGL GLSL source (ES 1.00 or ES 3.00) to Vulkan GLSL (#version 450).
    static TranslatedShader translateToVulkanGLSL(const std::string& glslSource, GLenum shaderType);

    /// Link vertex and fragment shader sources, verifying interface and unifying layout.
    static ProgramLinkResult linkShaders(const std::string& vsSource, const std::string& fsSource,
                                         const std::unordered_map<std::string, GLuint>& boundAttribs);

    /// Compile Vulkan GLSL source string to SPIR-V using glslc.
    static std::vector<uint32_t> compileToSpirv(const std::string& source,
                                                VkShaderStageFlagBits stage,
                                                std::string* outLog = nullptr);

    /// Create a VkShaderModule from SPIR-V bytecode.
    static VkShaderModule createShaderModule(VkDevice device, const std::vector<uint32_t>& spirv);

    /// Destroy a previously created VkShaderModule.
    static void destroyShaderModule(VkDevice device, VkShaderModule module);

private:
    static std::unordered_map<std::string, std::vector<uint32_t>> s_spirvCache;
};

} // namespace bro::webgl::vk
