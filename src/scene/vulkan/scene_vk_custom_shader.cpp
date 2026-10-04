#include "scene/vulkan/scene_vk_custom_shader.h"
#include "scene/vulkan/scene_vk_shader_compiler.h"
#include "scene/vulkan/scene_vk_pipeline.h"
#include "util/log.h"

#include <sstream>
#include <regex>
#include <algorithm>
#include <cstring>

#include "mesh.vert.src.h"
#include "mesh.frag.src.h"
#include "mesh_instanced.vert.src.h"
#include "mesh_skinned.vert.src.h"
#include "shadow.vert.src.h"
#include "shadow_skinned.vert.src.h"

namespace bro::scene::vk {

namespace {

std::string spliceChunk(const char* baseSrc, const std::string& chunk, const char* defineName) {
    std::string s(baseSrc ? baseSrc : "");
    if (chunk.empty()) return s;
    const char* marker = "//__USER_CHUNK__";
    size_t pos = s.find(marker);
    if (pos != std::string::npos) {
        std::string inject = std::string("\n#define ") + defineName + " 1\n" + chunk + "\n";
        s.replace(pos, std::strlen(marker), inject);
    }
    return s;
}

} // namespace

std::string SceneVkCustomShader::preprocessUserGlsl(const std::string& chunk,
                                                    uint32_t setIndex,
                                                    std::vector<std::string>& outSamplerNames) {
    if (chunk.empty()) return "";

    std::istringstream stream(chunk);
    std::string line;
    std::string uniformMembers;
    std::string samplerDecls;
    std::string body;

    static const std::regex uniformRegex(R"(^\s*uniform\s+([A-Za-z0-9_]+)\s+([A-Za-z0-9_]+)\s*;)");
    static const std::regex uniformArrayRegex(R"(^\s*uniform\s+([A-Za-z0-9_]+)\s+([A-Za-z0-9_]+)\s*\[\s*([0-9]+)\s*\]\s*;)");

    while (std::getline(stream, line)) {
        std::smatch match;
        if (std::regex_search(line, match, uniformRegex)) {
            std::string type = match[1].str();
            std::string name = match[2].str();
            if (type.rfind("sampler", 0) == 0) {
                auto it = std::find(outSamplerNames.begin(), outSamplerNames.end(), name);
                uint32_t binding = 1;
                if (it != outSamplerNames.end()) {
                    binding = static_cast<uint32_t>(1 + std::distance(outSamplerNames.begin(), it));
                } else {
                    outSamplerNames.push_back(name);
                    binding = static_cast<uint32_t>(outSamplerNames.size());
                }
                samplerDecls += "layout(set = " + std::to_string(setIndex) + ", binding = " +
                                std::to_string(binding) + ") uniform " + type + " " + name + ";\n";
            } else {
                uniformMembers += "    " + type + " " + name + ";\n";
            }
        } else if (std::regex_search(line, match, uniformArrayRegex)) {
            std::string type = match[1].str();
            std::string name = match[2].str();
            std::string count = match[3].str();
            if (type.rfind("sampler", 0) == 0) {
                auto it = std::find(outSamplerNames.begin(), outSamplerNames.end(), name);
                uint32_t binding = 1;
                if (it != outSamplerNames.end()) {
                    binding = static_cast<uint32_t>(1 + std::distance(outSamplerNames.begin(), it));
                } else {
                    outSamplerNames.push_back(name);
                    binding = static_cast<uint32_t>(outSamplerNames.size());
                }
                samplerDecls += "layout(set = " + std::to_string(setIndex) + ", binding = " +
                                std::to_string(binding) + ") uniform " + type + " " + name + "[" + count + "];\n";
            } else {
                uniformMembers += "    " + type + " " + name + "[" + count + "];\n";
            }
        } else {
            body += line + "\n";
        }
    }

    std::string res;
    if (!uniformMembers.empty()) {
        res += "layout(set = " + std::to_string(setIndex) + ", binding = 0, std140) uniform UserUniforms {\n" +
               uniformMembers +
               "};\n";
    }
    res += samplerDecls;
    res += body;
    return res;
}

std::vector<std::pair<std::string, uint32_t>> SceneVkCustomShader::parseUniformOffsets(const std::string& chunk,
                                                                                       uint32_t& outTotalSize) {
    std::vector<std::pair<std::string, uint32_t>> offsets;
    outTotalSize = 0;
    if (chunk.empty()) return offsets;

    std::istringstream stream(chunk);
    std::string line;
    static const std::regex uniformRegex(R"(^\s*uniform\s+([A-Za-z0-9_]+)\s+([A-Za-z0-9_]+)\s*;)");

    uint32_t curOffset = 0;
    while (std::getline(stream, line)) {
        std::smatch match;
        if (std::regex_search(line, match, uniformRegex)) {
            std::string type = match[1].str();
            std::string name = match[2].str();
            if (type.rfind("sampler", 0) == 0) continue;

            uint32_t size = 4;
            uint32_t align = 4;
            if (type == "vec2" || type == "ivec2" || type == "uvec2" || type == "bvec2") {
                size = 8; align = 8;
            } else if (type == "vec3" || type == "ivec3" || type == "uvec3") {
                size = 12; align = 16;
            } else if (type == "vec4" || type == "ivec4" || type == "uvec4") {
                size = 16; align = 16;
            } else if (type == "mat3") {
                size = 48; align = 16;
            } else if (type == "mat4") {
                size = 64; align = 16;
            }

            curOffset = (curOffset + align - 1) & ~(align - 1);
            offsets.push_back({name, curOffset});
            curOffset += size;
        }
    }

    outTotalSize = (curOffset + 15) & ~15;
    if (outTotalSize == 0) outTotalSize = 16;
    return offsets;
}

bool SceneVkCustomShader::validateCustomShader(SceneRenderer::CustomShaderTarget target,
                                               const std::string& vertexChunk,
                                               const std::string& fragmentChunk,
                                               std::string& errOut) {
    if (!vertexChunk.empty()) {
        std::vector<std::string> samplers;
        std::string pre = preprocessUserGlsl(vertexChunk, 4, samplers);
        std::string vsSrc;
        if (target == SceneRenderer::CustomShaderTarget::Instanced) {
            vsSrc = spliceChunk(kVkMeshInstancedVertSrc, pre, "CUSTOM_VERTEX");
        } else if (target == SceneRenderer::CustomShaderTarget::Skinned) {
            vsSrc = spliceChunk(kVkMeshSkinnedVertSrc, pre, "CUSTOM_VERTEX");
        } else {
            vsSrc = spliceChunk(kVkMeshVertSrc, pre, "CUSTOM_VERTEX");
        }
        auto spirv = SceneVkShaderCompiler::compileGlsl(vsSrc, VK_SHADER_STAGE_VERTEX_BIT, "main", {}, &errOut);
        if (spirv.empty()) {
            return false;
        }
    }
    if (!fragmentChunk.empty()) {
        std::vector<std::string> samplers;
        std::string pre = preprocessUserGlsl(fragmentChunk, 4, samplers);
        std::string fsSrc = spliceChunk(kVkMeshFragSrc, pre, "CUSTOM_FRAGMENT");
        auto spirv = SceneVkShaderCompiler::compileGlsl(fsSrc, VK_SHADER_STAGE_FRAGMENT_BIT, "main", {}, &errOut);
        if (spirv.empty()) {
            return false;
        }
    }
    return true;
}

bool SceneVkCustomShader::compileCustomShaderModules(VkDevice device,
                                                     SceneRenderer::CustomShaderTarget target,
                                                     const std::string& vertexChunk,
                                                     const std::string& fragmentChunk,
                                                     VkShaderModule& outVs,
                                                     VkShaderModule& outFs,
                                                     std::vector<std::string>& outSamplerNames,
                                                     std::string& errOut) {
    // Vertex stage
    std::string vsSrc;
    if (!vertexChunk.empty()) {
        std::string preVs = preprocessUserGlsl(vertexChunk, 4, outSamplerNames);
        if (target == SceneRenderer::CustomShaderTarget::Instanced) {
            vsSrc = spliceChunk(kVkMeshInstancedVertSrc, preVs, "CUSTOM_VERTEX");
        } else if (target == SceneRenderer::CustomShaderTarget::Skinned) {
            vsSrc = spliceChunk(kVkMeshSkinnedVertSrc, preVs, "CUSTOM_VERTEX");
        } else {
            vsSrc = spliceChunk(kVkMeshVertSrc, preVs, "CUSTOM_VERTEX");
        }
    } else {
        if (target == SceneRenderer::CustomShaderTarget::Instanced) {
            vsSrc = kVkMeshInstancedVertSrc;
        } else if (target == SceneRenderer::CustomShaderTarget::Skinned) {
            vsSrc = kVkMeshSkinnedVertSrc;
        } else {
            vsSrc = kVkMeshVertSrc;
        }
    }

    auto vsSpirv = SceneVkShaderCompiler::compileGlsl(vsSrc, VK_SHADER_STAGE_VERTEX_BIT, "main", {}, &errOut);
    if (vsSpirv.empty()) {
        return false;
    }

    // Fragment stage
    std::string fsSrc;
    if (!fragmentChunk.empty()) {
        std::string preFs = preprocessUserGlsl(fragmentChunk, 4, outSamplerNames);
        fsSrc = spliceChunk(kVkMeshFragSrc, preFs, "CUSTOM_FRAGMENT");
    } else {
        fsSrc = kVkMeshFragSrc;
    }

    auto fsSpirv = SceneVkShaderCompiler::compileGlsl(fsSrc, VK_SHADER_STAGE_FRAGMENT_BIT, "main", {}, &errOut);
    if (fsSpirv.empty()) {
        return false;
    }

    outVs = SceneVkShaderModule::create(device, vsSpirv);
    outFs = SceneVkShaderModule::create(device, fsSpirv);
    return outVs != VK_NULL_HANDLE && outFs != VK_NULL_HANDLE;
}

bool SceneVkCustomShader::compileCustomShadowShaderModule(VkDevice device,
                                                          bool isSkinned,
                                                          const std::string& vertexChunk,
                                                          VkShaderModule& outVs,
                                                          std::string& errOut) {
    if (vertexChunk.empty()) return false;
    std::vector<std::string> samplers;
    std::string preVs = preprocessUserGlsl(vertexChunk, 4, samplers);
    std::string vsSrc = spliceChunk(isSkinned ? kVkShadowSkinnedVertSrc : kVkShadowVertSrc,
                                    preVs, "CUSTOM_VERTEX");
    auto spirv = SceneVkShaderCompiler::compileGlsl(vsSrc, VK_SHADER_STAGE_VERTEX_BIT, "main", {}, &errOut);
    if (spirv.empty()) {
        return false;
    }
    outVs = SceneVkShaderModule::create(device, spirv);
    return outVs != VK_NULL_HANDLE;
}

} // namespace bro::scene::vk
