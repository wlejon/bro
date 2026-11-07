// What a linked program reports about itself: attribute, uniform, block and
// output queries, all answered from glslang's reflection (ProgramInterface)
// by the plain lookups of webgl_program_lookup.h; here, the program's
// existence and link status, and the errors.

#include "webgl/vulkan/webgl_vk_context.h"
#include "webgl/vulkan/webgl_program_lookup.h"

namespace bro::webgl::vk {

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
    return lookupAttribLocation(prog->iface, name);
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
    return lookupFragDataLocation(prog->iface, name);
}

WebGLUniformLocation WebGLVkContext::getUniformLocation(WebGLProgram p, const std::string& name) {
    const VkProgramResource* prog = linkedProgram(p);
    if (!prog || !prog->linkStatus) {
        if (prog) setSyntheticError(GL_INVALID_OPERATION);
        return {-1, p.id};
    }
    return {lookupUniformLocation(prog->iface, name), p.id};
}

GLuint WebGLVkContext::getUniformBlockIndex(WebGLProgram p, const std::string& name) {
    const VkProgramResource* prog = linkedProgram(p);
    return prog ? lookupUniformBlockIndex(prog->iface, name) : GL_INVALID_INDEX;
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
    bindingsChanged();
}

WebGLActiveInfo WebGLVkContext::getActiveAttrib(WebGLProgram p, GLuint index) {
    const VkProgramResource* prog = linkedProgram(p);
    WebGLActiveInfo info{"", 0, 0};
    if (!prog) return info;
    if (const GLenum err = lookupActiveAttrib(prog->iface, index, info)) setSyntheticError(err);
    return info;
}

WebGLActiveInfo WebGLVkContext::getActiveUniform(WebGLProgram p, GLuint index) {
    const VkProgramResource* prog = linkedProgram(p);
    WebGLActiveInfo info{"", 0, 0};
    if (!prog) return info;
    if (const GLenum err = lookupActiveUniform(prog->iface, index, info)) setSyntheticError(err);
    return info;
}

std::vector<GLuint> WebGLVkContext::getUniformIndices(WebGLProgram p, const std::vector<std::string>& names) {
    const VkProgramResource* prog = linkedProgram(p);
    if (!prog) return std::vector<GLuint>(names.size(), GL_INVALID_INDEX);
    return lookupUniformIndices(prog->iface, names);
}

std::vector<GLint> WebGLVkContext::getActiveUniforms(WebGLProgram p, const std::vector<GLuint>& indices,
                                                     GLenum pname) {
    const VkProgramResource* prog = linkedProgram(p);
    if (!prog) return std::vector<GLint>(indices.size(), 0);
    std::vector<GLint> res;
    if (const GLenum err = lookupActiveUniforms(prog->iface, indices, pname, res)) setSyntheticError(err);
    return res;
}

GLint WebGLVkContext::getActiveUniformBlockParameteri(WebGLProgram p, GLuint blockIndex, GLenum pname) {
    const VkProgramResource* prog = linkedProgram(p);
    if (!prog) return 0;
    // The binding point is program state (uniformBlockBinding), not interface.
    if (pname == GL_UNIFORM_BLOCK_BINDING && blockIndex < prog->iface.uniformBlocks.size())
        return static_cast<GLint>(prog->blockBindings[blockIndex]);
    GLint value = 0;
    if (const GLenum err = lookupUniformBlockParameter(prog->iface, blockIndex, pname, value)) setSyntheticError(err);
    return value;
}

std::vector<GLint> WebGLVkContext::getActiveUniformBlockIndices(WebGLProgram p, GLuint blockIndex) {
    const VkProgramResource* prog = linkedProgram(p);
    return prog ? lookupUniformBlockIndices(prog->iface, blockIndex) : std::vector<GLint>{};
}

std::string WebGLVkContext::getActiveUniformBlockName(WebGLProgram p, GLuint blockIndex) {
    const VkProgramResource* prog = linkedProgram(p);
    std::string name;
    if (!prog) return name;
    if (const GLenum err = lookupUniformBlockName(prog->iface, blockIndex, name)) setSyntheticError(err);
    return name;
}

} // namespace bro::webgl::vk
