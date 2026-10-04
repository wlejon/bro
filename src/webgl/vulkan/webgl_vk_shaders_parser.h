#pragma once

#include "webgl/vulkan/webgl_vk_types.h"
#include <string>
#include <vector>
#include <unordered_map>
#include <set>

namespace bro::webgl::vk {

struct ParsedVar {
    std::string name;
    std::string type;
    std::string qualifiers; // e.g. "flat", "centroid"
    int location = -1;      // -1 if not specified
    int arraySize = 1;      // 1 if not an array
    bool isArray = false;
};

struct ParsedUniformBlock {
    std::string name;
    std::string body;
    int binding = -1;
    std::vector<ParsedVar> members;
};

struct ParsedShader {
    GLenum type = 0; // GL_VERTEX_SHADER or GL_FRAGMENT_SHADER
    std::string cleanedSource;
    std::vector<ParsedVar> attributes;
    std::vector<ParsedVar> varyings;
    std::vector<ParsedVar> fragmentOutputs;
    std::vector<ParsedVar> uniforms;
    std::vector<ParsedVar> samplers;
    std::vector<ParsedUniformBlock> uniformBlocks;
    bool hasFragColor = false;
    bool hasFragCoord = false;
    bool hasPointCoord = false;
};

struct ProgramLinkResult {
    bool success = false;
    std::string errorLog;
    std::string vsVulkanSource;
    std::string fsVulkanSource;
    std::unordered_map<std::string, GLint> attribLocations;
    std::vector<VkAttribInfo> activeAttribs;
    std::unordered_map<std::string, GLint> fragDataLocations;
    std::vector<VkUniformInfo> uniforms;
    std::unordered_map<std::string, GLint> uniformLocations;
    std::unordered_map<std::string, GLint> samplerBindings;
    std::vector<VkUniformBlockInfo> uniformBlocks;
    std::unordered_map<std::string, GLuint> uniformBlockIndices;
    uint32_t pushConstantSize = 0;
};

class WebGLVkShaderParser {
public:
    static ParsedShader parse(const std::string& glslSource, GLenum shaderType);

    static std::string generateStandaloneVulkanGLSL(const ParsedShader& parsed);

    static ProgramLinkResult linkAndGenerateVulkanGLSL(
        const ParsedShader& vsParsed,
        const ParsedShader& fsParsed,
        const std::unordered_map<std::string, GLuint>& boundAttribs);

    static bool isSamplerType(const std::string& type);
    static GLenum typeStringToGLenum(const std::string& type);
    static std::pair<uint32_t, uint32_t> getUniformSizeAndAlign(const std::string& type);
    static uint32_t alignTo(uint32_t offset, uint32_t alignment);
};

} // namespace bro::webgl::vk
