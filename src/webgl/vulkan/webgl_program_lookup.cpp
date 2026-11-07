// Program interface lookups (webgl_program_lookup.h).

#include "webgl/vulkan/webgl_program_lookup.h"

#include <algorithm>
#include <cstdlib>
#include <utility>

namespace bro::webgl::vk {

namespace {

// "name[3]" -> ("name", 3); a name without a trailing subscript -> (name, -1).
std::pair<std::string, long> splitSubscript(const std::string& name) {
    if (name.empty() || name.back() != ']') return {name, -1};
    const size_t open = name.rfind('[');
    if (open == std::string::npos || open + 2 > name.size() - 1) return {name, -1};
    const std::string digits = name.substr(open + 1, name.size() - open - 2);
    if (digits.find_first_not_of("0123456789") != std::string::npos) return {name, -1};
    if (digits.size() > 1 && digits[0] == '0') return {name, -1};
    return {name.substr(0, open), std::strtol(digits.c_str(), nullptr, 10)};
}

} // namespace

GLint lookupAttribLocation(const ProgramInterface& iface, const std::string& name) {
    for (const VkAttribInfo& a : iface.attribs)
        if (a.name == name) return a.location;
    return -1;
}

GLint lookupFragDataLocation(const ProgramInterface& iface, const std::string& name) {
    auto [base, element] = splitSubscript(name);
    auto it = iface.fragDataLocations.find(element >= 0 ? base : name);
    if (it == iface.fragDataLocations.end()) return -1;
    return it->second + static_cast<GLint>(std::max(0L, element));
}

GLint lookupUniformLocation(const ProgramInterface& iface, const std::string& name) {
    auto [base, element] = splitSubscript(name);
    for (const VkUniformInfo& u : iface.uniforms) {
        if (u.location < 0) continue;
        if (u.size == 1 && u.name == name) return u.location;
        if (u.size > 1) {
            const std::string arrayBase = u.name.substr(0, u.name.size() - 3);  // drop "[0]"
            if (name == arrayBase) return u.location;
            if (element >= 0 && base == arrayBase && element < u.size)
                return u.location + static_cast<GLint>(element);
        }
    }
    return -1;
}

GLuint lookupUniformBlockIndex(const ProgramInterface& iface, const std::string& name) {
    for (size_t i = 0; i < iface.uniformBlocks.size(); ++i) {
        const std::string& blockName = iface.uniformBlocks[i].name;
        if (blockName == name || blockName == name + "[0]") return static_cast<GLuint>(i);
    }
    return GL_INVALID_INDEX;
}

GLenum lookupActiveAttrib(const ProgramInterface& iface, GLuint index, WebGLActiveInfo& out) {
    if (index >= iface.attribs.size()) return GL_INVALID_VALUE;
    const VkAttribInfo& a = iface.attribs[index];
    out = {a.name, a.type, a.size};
    return GL_NO_ERROR;
}

GLenum lookupActiveUniform(const ProgramInterface& iface, GLuint index, WebGLActiveInfo& out) {
    if (index >= iface.uniforms.size()) return GL_INVALID_VALUE;
    const VkUniformInfo& u = iface.uniforms[index];
    out = {u.name, u.type, u.size};
    return GL_NO_ERROR;
}

std::vector<GLuint> lookupUniformIndices(const ProgramInterface& iface, const std::vector<std::string>& names) {
    std::vector<GLuint> res(names.size(), GL_INVALID_INDEX);
    for (size_t i = 0; i < names.size(); ++i) {
        for (size_t u = 0; u < iface.uniforms.size(); ++u) {
            const std::string& uName = iface.uniforms[u].name;
            if (uName == names[i] || uName == names[i] + "[0]") {
                res[i] = static_cast<GLuint>(u);
                break;
            }
        }
    }
    return res;
}

GLenum lookupActiveUniforms(const ProgramInterface& iface, const std::vector<GLuint>& indices, GLenum pname,
                            std::vector<GLint>& out) {
    out.assign(indices.size(), 0);
    for (size_t i = 0; i < indices.size(); ++i) {
        if (indices[i] >= iface.uniforms.size()) {
            out.clear();
            return GL_INVALID_VALUE;
        }
        const VkUniformInfo& u = iface.uniforms[indices[i]];
        // GL reports no layout for default-block values.
        const bool inBlock = u.blockIndex >= 0;
        switch (pname) {
            case GL_UNIFORM_TYPE: out[i] = static_cast<GLint>(u.type); break;
            case GL_UNIFORM_SIZE: out[i] = u.size; break;
            case GL_UNIFORM_BLOCK_INDEX: out[i] = u.blockIndex; break;
            case GL_UNIFORM_OFFSET: out[i] = inBlock ? static_cast<GLint>(u.offset) : -1; break;
            case GL_UNIFORM_ARRAY_STRIDE: out[i] = inBlock ? static_cast<GLint>(u.arrayStride) : -1; break;
            case GL_UNIFORM_MATRIX_STRIDE: out[i] = inBlock ? static_cast<GLint>(u.matrixStride) : -1; break;
            case GL_UNIFORM_IS_ROW_MAJOR: out[i] = u.rowMajor ? 1 : 0; break;
            default:
                out.clear();
                return GL_INVALID_ENUM;
        }
    }
    return GL_NO_ERROR;
}

GLenum lookupUniformBlockParameter(const ProgramInterface& iface, GLuint blockIndex, GLenum pname, GLint& out) {
    out = 0;
    if (blockIndex >= iface.uniformBlocks.size()) return GL_INVALID_VALUE;
    const VkUniformBlockInfo& b = iface.uniformBlocks[blockIndex];
    switch (pname) {
        case GL_UNIFORM_BLOCK_DATA_SIZE: out = static_cast<GLint>(b.dataSize); return GL_NO_ERROR;
        case GL_UNIFORM_BLOCK_ACTIVE_UNIFORMS: out = static_cast<GLint>(b.activeUniformIndices.size()); return GL_NO_ERROR;
        case GL_UNIFORM_BLOCK_REFERENCED_BY_VERTEX_SHADER: out = b.referencedByVertex ? 1 : 0; return GL_NO_ERROR;
        case GL_UNIFORM_BLOCK_REFERENCED_BY_FRAGMENT_SHADER: out = b.referencedByFragment ? 1 : 0; return GL_NO_ERROR;
        default: return GL_INVALID_ENUM;
    }
}

std::vector<GLint> lookupUniformBlockIndices(const ProgramInterface& iface, GLuint blockIndex) {
    if (blockIndex >= iface.uniformBlocks.size()) return {};
    const VkUniformBlockInfo& b = iface.uniformBlocks[blockIndex];
    return std::vector<GLint>(b.activeUniformIndices.begin(), b.activeUniformIndices.end());
}

GLenum lookupUniformBlockName(const ProgramInterface& iface, GLuint blockIndex, std::string& out) {
    if (blockIndex >= iface.uniformBlocks.size()) return GL_INVALID_VALUE;
    out = iface.uniformBlocks[blockIndex].name;
    return GL_NO_ERROR;
}

} // namespace bro::webgl::vk
