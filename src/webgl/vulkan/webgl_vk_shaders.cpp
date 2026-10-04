#include "webgl/vulkan/webgl_vk_shaders.h"
#include "util/log.h"

#include "util/subprocess.h"

#include <sstream>
#include <regex>
#include <cstring>
#include <mutex>

#if BRO_HAS_SHADERC
#include <shaderc/shaderc.hpp>
#endif

namespace bro::webgl::vk {

namespace {

std::mutex s_compilerMutex;

} // namespace

std::unordered_map<std::string, std::vector<uint32_t>> WebGLVkShaderCompiler::s_spirvCache;

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
    std::lock_guard<std::mutex> lock(s_compilerMutex);

    std::string stageStr = (stage == VK_SHADER_STAGE_VERTEX_BIT) ? "vertex" : "fragment";
    std::string cacheKey = stageStr + "|" + source;

    auto it = s_spirvCache.find(cacheKey);
    if (it != s_spirvCache.end()) {
        return it->second;
    }

#if BRO_HAS_SHADERC
    shaderc::Compiler compiler;
    if (compiler.IsValid()) {
        shaderc::CompileOptions options;
        options.SetTargetEnvironment(shaderc_target_env_vulkan, shaderc_env_version_vulkan_1_0);
        shaderc_shader_kind kind = (stage == VK_SHADER_STAGE_VERTEX_BIT) ? shaderc_vertex_shader : shaderc_fragment_shader;
        auto result = compiler.CompileGlslToSpv(source.c_str(), source.size(), kind, "webgl_shader", "main", options);
        if (result.GetCompilationStatus() == shaderc_compilation_status_success) {
            std::vector<uint32_t> spirv(result.cbegin(), result.cend());
            if (!spirv.empty() && spirv[0] == 0x07230203) {
                s_spirvCache[cacheKey] = spirv;
                return spirv;
            }
        } else {
            std::string errStr = result.GetErrorMessage();
            if (outLog) *outLog = errStr;
            LOG_ERROR("WebGLVkShaderCompiler: Compilation failed for %s shader:\n%s\nErrors:\n%s",
                      stageStr.c_str(), source.c_str(), errStr.c_str());
            return {};
        }
    }
#endif

    if (!util::hasExecutableOnPath("glslc")) {
        std::string errStr = "glslc not found on system PATH";
        if (outLog) *outLog = errStr;
        LOG_ERROR("WebGLVkShaderCompiler: %s", errStr.c_str());
        return {};
    }

    std::vector<std::string> args = { "glslc", "-fshader-stage=" + stageStr, "-", "-o", "-" };
    auto res = util::runSubprocess(args, source);
    if (res.success && res.stdOut.size() >= 4 && (res.stdOut.size() % 4 == 0)) {
        size_t wordCount = res.stdOut.size() / 4;
        std::vector<uint32_t> spirv(wordCount);
        std::memcpy(spirv.data(), res.stdOut.data(), res.stdOut.size());
        if (spirv[0] == 0x07230203) {
            s_spirvCache[cacheKey] = spirv;
            return spirv;
        }
    }

    if (outLog) {
        *outLog = res.stdErr;
    }
    LOG_ERROR("WebGLVkShaderCompiler: Compilation failed for %s shader:\n%s\nErrors:\n%s",
              stageStr.c_str(), source.c_str(), res.stdErr.c_str());
    return {};
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
