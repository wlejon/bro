// What a linked program reports about itself: attribute, uniform, block and
// output queries, all answered from glslang's reflection (ProgramInterface).

#include "webgl/vulkan/webgl_vk_context.h"

#include <cstdlib>

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

const VkProgramResource* WebGLVkContext::linkedProgram(WebGLProgram p) const {
    auto it = programs_.find(p.id);
    if (it == programs_.end()) return nullptr;
    return &it->second;
}

GLint WebGLVkContext::getAttribLocation(WebGLProgram p, const std::string& name) {
    const VkProgramResource* prog = linkedProgram(p);
    if (!prog || !prog->linkStatus) {
        if (prog) setSyntheticError(GL_INVALID_OPERATION);
        return -1;
    }
    for (const VkAttribInfo& a : prog->iface.attribs)
        if (a.name == name) return a.location;
    return -1;
}

void WebGLVkContext::bindAttribLocation(WebGLProgram p, GLuint index, const std::string& name) {
    auto it = programs_.find(p.id);
    if (it == programs_.end()) return;
    if (index >= static_cast<GLuint>(getParameterInt(GL_MAX_VERTEX_ATTRIBS))) {
        setSyntheticError(GL_INVALID_VALUE);
        return;
    }
    if (name.rfind("gl_", 0) == 0) {
        setSyntheticError(GL_INVALID_OPERATION);
        return;
    }
    // Takes effect at the next link.
    it->second.boundAttribLocations[name] = index;
}

GLint WebGLVkContext::getFragDataLocation(WebGLProgram p, const std::string& name) {
    const VkProgramResource* prog = linkedProgram(p);
    if (!prog || !prog->linkStatus) {
        if (prog) setSyntheticError(GL_INVALID_OPERATION);
        return -1;
    }
    auto [base, element] = splitSubscript(name);
    auto it = prog->iface.fragDataLocations.find(element >= 0 ? base : name);
    if (it == prog->iface.fragDataLocations.end()) return -1;
    return it->second + static_cast<GLint>(std::max(0L, element));
}

WebGLUniformLocation WebGLVkContext::getUniformLocation(WebGLProgram p, const std::string& name) {
    const VkProgramResource* prog = linkedProgram(p);
    if (!prog || !prog->linkStatus) {
        if (prog) setSyntheticError(GL_INVALID_OPERATION);
        return {-1, p.id};
    }
    // "u" and "u[0]" name an array's first element, "u[k]" its k-th; block
    // members have no location.
    auto [base, element] = splitSubscript(name);
    for (const VkUniformInfo& u : prog->iface.uniforms) {
        if (u.location < 0) continue;
        if (u.size == 1 && u.name == name) return {u.location, p.id};
        if (u.size > 1) {
            const std::string arrayBase = u.name.substr(0, u.name.size() - 3);  // drop "[0]"
            if (name == arrayBase) return {u.location, p.id};
            if (element >= 0 && base == arrayBase && element < u.size)
                return {u.location + static_cast<GLint>(element), p.id};
        }
    }
    return {-1, p.id};
}

GLuint WebGLVkContext::getUniformBlockIndex(WebGLProgram p, const std::string& name) {
    const VkProgramResource* prog = linkedProgram(p);
    if (!prog) return GL_INVALID_INDEX;
    for (size_t i = 0; i < prog->iface.uniformBlocks.size(); ++i) {
        const std::string& blockName = prog->iface.uniformBlocks[i].name;
        // An array of blocks is named "Block[0]"; "Block" is its first element.
        if (blockName == name || blockName == name + "[0]") return static_cast<GLuint>(i);
    }
    return GL_INVALID_INDEX;
}

void WebGLVkContext::uniformBlockBinding(WebGLProgram p, GLuint blockIndex, GLuint bindingPoint) {
    auto it = programs_.find(p.id);
    if (it == programs_.end()) return;
    VkProgramResource& prog = it->second;
    if (blockIndex >= prog.blockBindings.size() ||
        bindingPoint >= static_cast<GLuint>(getParameterInt(GL_MAX_UNIFORM_BUFFER_BINDINGS))) {
        setSyntheticError(GL_INVALID_VALUE);
        return;
    }
    prog.blockBindings[blockIndex] = bindingPoint;
}

WebGLActiveInfo WebGLVkContext::getActiveAttrib(WebGLProgram p, GLuint index) {
    const VkProgramResource* prog = linkedProgram(p);
    if (!prog) return {"", 0, 0};
    if (index >= prog->iface.attribs.size()) {
        setSyntheticError(GL_INVALID_VALUE);
        return {"", 0, 0};
    }
    const VkAttribInfo& a = prog->iface.attribs[index];
    return {a.name, a.type, a.size};
}

WebGLActiveInfo WebGLVkContext::getActiveUniform(WebGLProgram p, GLuint index) {
    const VkProgramResource* prog = linkedProgram(p);
    if (!prog) return {"", 0, 0};
    if (index >= prog->iface.uniforms.size()) {
        setSyntheticError(GL_INVALID_VALUE);
        return {"", 0, 0};
    }
    const VkUniformInfo& u = prog->iface.uniforms[index];
    return {u.name, u.type, u.size};
}

std::vector<GLuint> WebGLVkContext::getUniformIndices(WebGLProgram p, const std::vector<std::string>& names) {
    std::vector<GLuint> res(names.size(), GL_INVALID_INDEX);
    const VkProgramResource* prog = linkedProgram(p);
    if (!prog) return res;
    for (size_t i = 0; i < names.size(); ++i) {
        for (size_t u = 0; u < prog->iface.uniforms.size(); ++u) {
            const std::string& uName = prog->iface.uniforms[u].name;
            if (uName == names[i] || uName == names[i] + "[0]") {
                res[i] = static_cast<GLuint>(u);
                break;
            }
        }
    }
    return res;
}

std::vector<GLint> WebGLVkContext::getActiveUniforms(WebGLProgram p, const std::vector<GLuint>& indices,
                                                     GLenum pname) {
    std::vector<GLint> res(indices.size(), 0);
    const VkProgramResource* prog = linkedProgram(p);
    if (!prog) return res;
    for (size_t i = 0; i < indices.size(); ++i) {
        if (indices[i] >= prog->iface.uniforms.size()) {
            setSyntheticError(GL_INVALID_VALUE);
            return {};
        }
        const VkUniformInfo& u = prog->iface.uniforms[indices[i]];
        // GL reports no layout for default-block values.
        const bool inBlock = u.blockIndex >= 0;
        switch (pname) {
            case GL_UNIFORM_TYPE: res[i] = static_cast<GLint>(u.type); break;
            case GL_UNIFORM_SIZE: res[i] = u.size; break;
            case GL_UNIFORM_BLOCK_INDEX: res[i] = u.blockIndex; break;
            case GL_UNIFORM_OFFSET: res[i] = inBlock ? static_cast<GLint>(u.offset) : -1; break;
            case GL_UNIFORM_ARRAY_STRIDE: res[i] = inBlock ? static_cast<GLint>(u.arrayStride) : -1; break;
            case GL_UNIFORM_MATRIX_STRIDE: res[i] = inBlock ? static_cast<GLint>(u.matrixStride) : -1; break;
            case GL_UNIFORM_IS_ROW_MAJOR: res[i] = u.rowMajor ? 1 : 0; break;
            default:
                setSyntheticError(GL_INVALID_ENUM);
                return {};
        }
    }
    return res;
}

GLint WebGLVkContext::getActiveUniformBlockParameteri(WebGLProgram p, GLuint blockIndex, GLenum pname) {
    const VkProgramResource* prog = linkedProgram(p);
    if (!prog) return 0;
    if (blockIndex >= prog->iface.uniformBlocks.size()) {
        setSyntheticError(GL_INVALID_VALUE);
        return 0;
    }
    const VkUniformBlockInfo& b = prog->iface.uniformBlocks[blockIndex];
    switch (pname) {
        case GL_UNIFORM_BLOCK_BINDING: return static_cast<GLint>(prog->blockBindings[blockIndex]);
        case GL_UNIFORM_BLOCK_DATA_SIZE: return static_cast<GLint>(b.dataSize);
        case GL_UNIFORM_BLOCK_ACTIVE_UNIFORMS: return static_cast<GLint>(b.activeUniformIndices.size());
        case GL_UNIFORM_BLOCK_REFERENCED_BY_VERTEX_SHADER: return b.referencedByVertex ? 1 : 0;
        case GL_UNIFORM_BLOCK_REFERENCED_BY_FRAGMENT_SHADER: return b.referencedByFragment ? 1 : 0;
        default:
            setSyntheticError(GL_INVALID_ENUM);
            return 0;
    }
}

std::vector<GLint> WebGLVkContext::getActiveUniformBlockIndices(WebGLProgram p, GLuint blockIndex) {
    const VkProgramResource* prog = linkedProgram(p);
    if (!prog || blockIndex >= prog->iface.uniformBlocks.size()) return {};
    const VkUniformBlockInfo& b = prog->iface.uniformBlocks[blockIndex];
    return std::vector<GLint>(b.activeUniformIndices.begin(), b.activeUniformIndices.end());
}

std::string WebGLVkContext::getActiveUniformBlockName(WebGLProgram p, GLuint blockIndex) {
    const VkProgramResource* prog = linkedProgram(p);
    if (!prog) return "";
    if (blockIndex >= prog->iface.uniformBlocks.size()) {
        setSyntheticError(GL_INVALID_VALUE);
        return "";
    }
    return prog->iface.uniformBlocks[blockIndex].name;
}

} // namespace bro::webgl::vk
