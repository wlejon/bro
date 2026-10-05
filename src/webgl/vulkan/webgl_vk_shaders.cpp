#include "webgl/vulkan/webgl_vk_shaders.h"
#include "render/glsl_compiler.h"
#include "util/log.h"

#include <algorithm>

namespace bro::webgl::vk {

TranslatedShader WebGLVkShaderCompiler::translateToVulkanGLSL(const std::string& glslSource, GLenum shaderType) {
    ParsedShader parsed = WebGLVkShaderParser::parse(glslSource, shaderType);
    TranslatedShader result;
    result.source = WebGLVkShaderParser::generateStandaloneVulkanGLSL(parsed);

    int nextLoc = 0;
    for (const auto& attr : parsed.attributes) {
        int loc = (attr.location >= 0) ? attr.location : nextLoc;
        nextLoc = std::max(nextLoc, loc + 1);
        result.attributeLocations[attr.name] = loc;
    }
    int nextBinding = 0;
    for (const auto& s : parsed.samplers) {
        result.samplerBindings[s.name] = nextBinding++;
    }
    uint32_t currentOffset = 0;
    int nextUniLoc = 0;
    for (const auto& u : parsed.uniforms) {
        auto [baseSize, align] = WebGLVkShaderParser::getUniformSizeAndAlign(u.type);
        uint32_t totalSize = u.isArray ? (u.arraySize * WebGLVkShaderParser::alignTo(baseSize, 16)) : baseSize;
        currentOffset = WebGLVkShaderParser::alignTo(currentOffset, align);
        VkUniformInfo info;
        info.name = u.name + (u.isArray ? "[0]" : "");
        info.location = nextUniLoc++;
        info.type = WebGLVkShaderParser::typeStringToGLenum(u.type);
        info.offset = currentOffset;
        info.size = totalSize;
        info.count = u.arraySize;
        result.uniforms.push_back(info);
        currentOffset += totalSize;
    }
    result.defaultUniformSize = WebGLVkShaderParser::alignTo(currentOffset, 16);
    return result;
}

ProgramLinkResult WebGLVkShaderCompiler::linkShaders(const std::string& vsSource, const std::string& fsSource,
                                                     const std::unordered_map<std::string, GLuint>& boundAttribs)
{
    ParsedShader vsParsed = WebGLVkShaderParser::parse(vsSource, GL_VERTEX_SHADER);
    ParsedShader fsParsed = WebGLVkShaderParser::parse(fsSource, GL_FRAGMENT_SHADER);
    return WebGLVkShaderParser::linkAndGenerateVulkanGLSL(vsParsed, fsParsed, boundAttribs);
}

std::vector<uint32_t> WebGLVkShaderCompiler::compileToSpirv(const std::string& source,
                                                            VkShaderStageFlagBits stage,
                                                            std::string* outLog)
{
    const bool vertex = stage == VK_SHADER_STAGE_VERTEX_BIT;
    std::string log;
    std::vector<uint32_t> spirv = render::compileGlslToSpirv(
        source, vertex ? render::ShaderStage::Vertex : render::ShaderStage::Fragment, &log);
    if (spirv.empty()) {
        LOG_ERROR("WebGLVkShaderCompiler: Compilation failed for %s shader:\n%s\nErrors:\n%s",
                  vertex ? "vertex" : "fragment", source.c_str(), log.c_str());
    }
    if (outLog) *outLog = std::move(log);
    return spirv;
}

VkShaderModule WebGLVkShaderCompiler::createShaderModule(VkDevice device, const std::vector<uint32_t>& spirv) {
    if (device == VK_NULL_HANDLE || spirv.empty()) return VK_NULL_HANDLE;

    VkShaderModuleCreateInfo createInfo{};
    createInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    createInfo.codeSize = spirv.size() * sizeof(uint32_t);
    createInfo.pCode = spirv.data();

    VkShaderModule module = VK_NULL_HANDLE;
    if (vkCreateShaderModule(device, &createInfo, nullptr, &module) != VK_SUCCESS) {
        LOG_ERROR("WebGLVkShaderCompiler: Failed to create VkShaderModule");
        return VK_NULL_HANDLE;
    }
    return module;
}

void WebGLVkShaderCompiler::destroyShaderModule(VkDevice device, VkShaderModule module) {
    if (device != VK_NULL_HANDLE && module != VK_NULL_HANDLE) {
        vkDestroyShaderModule(device, module, nullptr);
    }
}

} // namespace bro::webgl::vk
