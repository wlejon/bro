#include "webgl/vulkan/webgl_vk_context.h"
#include "util/log.h"
#include <algorithm>
#include <cstring>

namespace bro::webgl::vk {

GLint WebGLVkContext::getAttribLocation(WebGLProgram p, const std::string& name) {
    auto it = programs_.find(p.id);
    if (it == programs_.end()) return -1;
    auto aIt = it->second.attribLocations.find(name);
    return (aIt != it->second.attribLocations.end()) ? aIt->second : -1;
}

void WebGLVkContext::bindAttribLocation(WebGLProgram p, GLuint index, const std::string& name) {
    auto it = programs_.find(p.id);
    if (it != programs_.end()) {
        it->second.boundAttribLocations[name] = index;
    }
}

GLint WebGLVkContext::getFragDataLocation(WebGLProgram p, const std::string& name) {
    auto it = programs_.find(p.id);
    if (it == programs_.end()) return -1;
    auto fit = it->second.fragDataLocations.find(name);
    if (fit != it->second.fragDataLocations.end()) return fit->second;
    return (name == "gl_FragColor") ? 0 : -1;
}

WebGLUniformLocation WebGLVkContext::getUniformLocation(WebGLProgram p, const std::string& name) {
    auto it = programs_.find(p.id);
    if (it == programs_.end()) return {-1, 0};
    auto uIt = it->second.uniformLocations.find(name);
    if (uIt != it->second.uniformLocations.end()) {
        return WebGLUniformLocation{uIt->second, p.id};
    }
    // Try array element "[0]"
    auto aIt = it->second.uniformLocations.find(name + "[0]");
    if (aIt != it->second.uniformLocations.end()) {
        return WebGLUniformLocation{aIt->second, p.id};
    }
    return WebGLUniformLocation{-1, p.id};
}

GLuint WebGLVkContext::getUniformBlockIndex(WebGLProgram p, const std::string& name) {
    auto it = programs_.find(p.id);
    if (it == programs_.end()) return GL_INVALID_INDEX;
    auto bit = it->second.uniformBlockIndices.find(name);
    return (bit != it->second.uniformBlockIndices.end()) ? bit->second : GL_INVALID_INDEX;
}

void WebGLVkContext::uniformBlockBinding(WebGLProgram p, GLuint blockIndex, GLuint bindingPoint) {
    auto it = programs_.find(p.id);
    if (it != programs_.end()) {
        if (blockIndex < it->second.uniformBlocks.size()) {
            it->second.uniformBlocks[blockIndex].binding = bindingPoint;
        }
        it->second.uniformBlockBindings[blockIndex] = bindingPoint;
    }
}

WebGLActiveInfo WebGLVkContext::getActiveAttrib(WebGLProgram p, GLuint index) {
    auto it = programs_.find(p.id);
    if (it != programs_.end() && index < it->second.activeAttribs.size()) {
        const auto& a = it->second.activeAttribs[index];
        return {a.name, static_cast<GLenum>(a.type), a.size};
    }
    return {"", 0, 0};
}

WebGLActiveInfo WebGLVkContext::getActiveUniform(WebGLProgram p, GLuint index) {
    auto it = programs_.find(p.id);
    if (it != programs_.end() && index < it->second.uniforms.size()) {
        const auto& u = it->second.uniforms[index];
        return {u.name, static_cast<GLenum>(u.type), u.count};
    }
    return {"", 0, 0};
}

// ---------------------------------------------------------------------------
// Vertex Attributes
// ---------------------------------------------------------------------------

void WebGLVkContext::vertexAttribIPointer(GLuint index, GLint size, GLenum type,
                                          GLsizei stride, uintptr_t offset) {
    if (index >= 16) return;
    VkVertexAttribute& attr = vaos_[currentVaoId_].attributes[index];
    attr.size = size;
    attr.type = type;
    attr.normalized = GL_FALSE;
    attr.isInteger = true;
    attr.stride = (stride == 0) ? (size * 4) : stride;
    attr.offset = offset;
    attr.bufferId = boundArrayBuffer_;
}

void WebGLVkContext::vertexAttrib1f(GLuint index, GLfloat x) { vertexAttrib4f(index, x, 0.0f, 0.0f, 1.0f); }
void WebGLVkContext::vertexAttrib2f(GLuint index, GLfloat x, GLfloat y) { vertexAttrib4f(index, x, y, 0.0f, 1.0f); }
void WebGLVkContext::vertexAttrib3f(GLuint index, GLfloat x, GLfloat y, GLfloat z) { vertexAttrib4f(index, x, y, z, 1.0f); }

void WebGLVkContext::vertexAttrib4f(GLuint index, GLfloat x, GLfloat y, GLfloat z, GLfloat w) {
    if (index >= 16) return;
    float* dst = reinterpret_cast<float*>(genericAttribs_[index].data());
    dst[0] = x; dst[1] = y; dst[2] = z; dst[3] = w;
    vaos_[currentVaoId_].attributes[index].isInteger = false;
    genericAttribsChanged();
}

void WebGLVkContext::vertexAttrib1fv(GLuint index, const GLfloat* v) { if (v) vertexAttrib1f(index, v[0]); }
void WebGLVkContext::vertexAttrib2fv(GLuint index, const GLfloat* v) { if (v) vertexAttrib2f(index, v[0], v[1]); }
void WebGLVkContext::vertexAttrib3fv(GLuint index, const GLfloat* v) { if (v) vertexAttrib3f(index, v[0], v[1], v[2]); }
void WebGLVkContext::vertexAttrib4fv(GLuint index, const GLfloat* v) { if (v) vertexAttrib4f(index, v[0], v[1], v[2], v[3]); }

void WebGLVkContext::vertexAttribI4i(GLuint index, GLint x, GLint y, GLint z, GLint w) {
    if (index >= 16) return;
    int32_t* dst = reinterpret_cast<int32_t*>(genericAttribs_[index].data());
    dst[0] = x; dst[1] = y; dst[2] = z; dst[3] = w;
    vaos_[currentVaoId_].attributes[index].isInteger = true;
    genericAttribsChanged();
}

void WebGLVkContext::vertexAttribI4ui(GLuint index, GLuint x, GLuint y, GLuint z, GLuint w) {
    if (index >= 16) return;
    uint32_t* dst = genericAttribs_[index].data();
    dst[0] = x; dst[1] = y; dst[2] = z; dst[3] = w;
    vaos_[currentVaoId_].attributes[index].isInteger = true;
    genericAttribsChanged();
}

void WebGLVkContext::vertexAttribI4iv(GLuint index, const GLint* v) {
    if (v) vertexAttribI4i(index, v[0], v[1], v[2], v[3]);
}

void WebGLVkContext::vertexAttribI4uiv(GLuint index, const GLuint* v) {
    if (v) vertexAttribI4ui(index, v[0], v[1], v[2], v[3]);
}

// ---------------------------------------------------------------------------
// Uniforms
// ---------------------------------------------------------------------------

namespace {

const VkUniformInfo* findUniform(const VkProgramResource& prog, GLint loc) {
    for (const auto& u : prog.uniforms) {
        if (u.location == loc) return &u;
    }
    return nullptr;
}

} // namespace

void WebGLVkContext::uniform1f(WebGLUniformLocation loc, GLfloat v0) { uniform1fv(loc, 1, &v0); }
void WebGLVkContext::uniform2f(WebGLUniformLocation loc, GLfloat v0, GLfloat v1) {
    GLfloat v[2] = {v0, v1}; uniform2fv(loc, 1, v);
}
void WebGLVkContext::uniform3f(WebGLUniformLocation loc, GLfloat v0, GLfloat v1, GLfloat v2) {
    GLfloat v[3] = {v0, v1, v2}; uniform3fv(loc, 1, v);
}
void WebGLVkContext::uniform4f(WebGLUniformLocation loc, GLfloat v0, GLfloat v1, GLfloat v2, GLfloat v3) {
    GLfloat v[4] = {v0, v1, v2, v3}; uniform4fv(loc, 1, v);
}

void WebGLVkContext::uniform1i(WebGLUniformLocation loc, GLint v0) { uniform1iv(loc, 1, &v0); }
void WebGLVkContext::uniform2i(WebGLUniformLocation loc, GLint v0, GLint v1) {
    GLint v[2] = {v0, v1}; uniform2iv(loc, 1, v);
}
void WebGLVkContext::uniform3i(WebGLUniformLocation loc, GLint v0, GLint v1, GLint v2) {
    GLint v[3] = {v0, v1, v2}; uniform3iv(loc, 1, v);
}
void WebGLVkContext::uniform4i(WebGLUniformLocation loc, GLint v0, GLint v1, GLint v2, GLint v3) {
    GLint v[4] = {v0, v1, v2, v3}; uniform4iv(loc, 1, v);
}

void WebGLVkContext::uniform1ui(WebGLUniformLocation loc, GLuint v0) { uniform1uiv(loc, 1, &v0); }
void WebGLVkContext::uniform2ui(WebGLUniformLocation loc, GLuint v0, GLuint v1) {
    GLuint v[2] = {v0, v1}; uniform2uiv(loc, 1, v);
}
void WebGLVkContext::uniform3ui(WebGLUniformLocation loc, GLuint v0, GLuint v1, GLuint v2) {
    GLuint v[3] = {v0, v1, v2}; uniform3uiv(loc, 1, v);
}
void WebGLVkContext::uniform4ui(WebGLUniformLocation loc, GLuint v0, GLuint v1, GLuint v2, GLuint v3) {
    GLuint v[4] = {v0, v1, v2, v3}; uniform4uiv(loc, 1, v);
}

template <typename T>
static void copyUniformArray(std::vector<uint8_t>& dstBytes, const VkUniformInfo& u, GLsizei count, const T* src, size_t elemCount) {
    if (u.arrayStride > 0) {
        GLsizei n = std::min(count, static_cast<GLsizei>(u.count));
        for (GLsizei i = 0; i < n; ++i) {
            size_t offset = u.offset + i * u.arrayStride;
            if (offset + elemCount * sizeof(T) <= dstBytes.size()) {
                std::memcpy(dstBytes.data() + offset, src + i * elemCount, elemCount * sizeof(T));
            }
        }
    } else {
        size_t bytes = std::min(static_cast<size_t>(count) * elemCount * sizeof(T), static_cast<size_t>(u.size));
        if (u.offset + bytes <= dstBytes.size()) {
            std::memcpy(dstBytes.data() + u.offset, src, bytes);
        }
    }
}

void WebGLVkContext::uniform1fv(WebGLUniformLocation loc, GLsizei count, const GLfloat* v) {
    if (loc.location < 0 || !v || count <= 0) return;
    auto it = programs_.find(currentProgramId_);
    if (it == programs_.end()) return;
    if (const auto* u = findUniform(it->second, loc.location)) {
        copyUniformArray(it->second.uniformBytes, *u, count, v, 1);
    }
}

void WebGLVkContext::uniform2fv(WebGLUniformLocation loc, GLsizei count, const GLfloat* v) {
    if (loc.location < 0 || !v || count <= 0) return;
    auto it = programs_.find(currentProgramId_);
    if (it == programs_.end()) return;
    if (const auto* u = findUniform(it->second, loc.location)) {
        copyUniformArray(it->second.uniformBytes, *u, count, v, 2);
    }
}

void WebGLVkContext::uniform3fv(WebGLUniformLocation loc, GLsizei count, const GLfloat* v) {
    if (loc.location < 0 || !v || count <= 0) return;
    auto it = programs_.find(currentProgramId_);
    if (it == programs_.end()) return;
    if (const auto* u = findUniform(it->second, loc.location)) {
        copyUniformArray(it->second.uniformBytes, *u, count, v, 3);
    }
}

void WebGLVkContext::uniform4fv(WebGLUniformLocation loc, GLsizei count, const GLfloat* v) {
    if (loc.location < 0 || !v || count <= 0) return;
    auto it = programs_.find(currentProgramId_);
    if (it == programs_.end()) return;
    if (const auto* u = findUniform(it->second, loc.location)) {
        copyUniformArray(it->second.uniformBytes, *u, count, v, 4);
    }
}

void WebGLVkContext::uniform1iv(WebGLUniformLocation loc, GLsizei count, const GLint* v) {
    if (loc.location < 0 || !v || count <= 0) return;
    auto it = programs_.find(currentProgramId_);
    if (it == programs_.end()) return;
    auto sIt = it->second.samplerLocToBinding.find(loc.location);
    if (sIt != it->second.samplerLocToBinding.end()) {
        it->second.samplerBindings[sIt->second] = static_cast<uint32_t>(std::max(0, v[0]));
        return;
    }
    if (const auto* u = findUniform(it->second, loc.location)) {
        copyUniformArray(it->second.uniformBytes, *u, count, v, 1);
    }
}

void WebGLVkContext::uniform2iv(WebGLUniformLocation loc, GLsizei count, const GLint* v) {
    if (loc.location < 0 || !v || count <= 0) return;
    auto it = programs_.find(currentProgramId_);
    if (it == programs_.end()) return;
    if (const auto* u = findUniform(it->second, loc.location)) {
        copyUniformArray(it->second.uniformBytes, *u, count, v, 2);
    }
}

void WebGLVkContext::uniform3iv(WebGLUniformLocation loc, GLsizei count, const GLint* v) {
    if (loc.location < 0 || !v || count <= 0) return;
    auto it = programs_.find(currentProgramId_);
    if (it == programs_.end()) return;
    if (const auto* u = findUniform(it->second, loc.location)) {
        copyUniformArray(it->second.uniformBytes, *u, count, v, 3);
    }
}

void WebGLVkContext::uniform4iv(WebGLUniformLocation loc, GLsizei count, const GLint* v) {
    if (loc.location < 0 || !v || count <= 0) return;
    auto it = programs_.find(currentProgramId_);
    if (it == programs_.end()) return;
    if (const auto* u = findUniform(it->second, loc.location)) {
        copyUniformArray(it->second.uniformBytes, *u, count, v, 4);
    }
}

void WebGLVkContext::uniform1uiv(WebGLUniformLocation loc, GLsizei count, const GLuint* v) {
    if (loc.location < 0 || !v || count <= 0) return;
    auto it = programs_.find(currentProgramId_);
    if (it == programs_.end()) return;
    if (const auto* u = findUniform(it->second, loc.location)) {
        copyUniformArray(it->second.uniformBytes, *u, count, v, 1);
    }
}

void WebGLVkContext::uniform2uiv(WebGLUniformLocation loc, GLsizei count, const GLuint* v) {
    if (loc.location < 0 || !v || count <= 0) return;
    auto it = programs_.find(currentProgramId_);
    if (it == programs_.end()) return;
    if (const auto* u = findUniform(it->second, loc.location)) {
        copyUniformArray(it->second.uniformBytes, *u, count, v, 2);
    }
}

void WebGLVkContext::uniform3uiv(WebGLUniformLocation loc, GLsizei count, const GLuint* v) {
    if (loc.location < 0 || !v || count <= 0) return;
    auto it = programs_.find(currentProgramId_);
    if (it == programs_.end()) return;
    if (const auto* u = findUniform(it->second, loc.location)) {
        copyUniformArray(it->second.uniformBytes, *u, count, v, 3);
    }
}

void WebGLVkContext::uniform4uiv(WebGLUniformLocation loc, GLsizei count, const GLuint* v) {
    if (loc.location < 0 || !v || count <= 0) return;
    auto it = programs_.find(currentProgramId_);
    if (it == programs_.end()) return;
    if (const auto* u = findUniform(it->second, loc.location)) {
        copyUniformArray(it->second.uniformBytes, *u, count, v, 4);
    }
}

void WebGLVkContext::uniformMatrix2fv(WebGLUniformLocation loc, GLsizei count, GLboolean /*transpose*/, const GLfloat* value) {
    if (loc.location < 0 || !value || count <= 0) return;
    auto it = programs_.find(currentProgramId_);
    if (it == programs_.end()) return;
    if (const auto* u = findUniform(it->second, loc.location)) {
        for (GLsizei c = 0; c < count; ++c) {
            size_t base = u->offset + c * 32;
            if (base + 24 <= it->second.uniformBytes.size()) {
                const float* src = value + c * 4;
                std::memcpy(it->second.uniformBytes.data() + base + 0, src + 0, 8);
                std::memcpy(it->second.uniformBytes.data() + base + 16, src + 2, 8);
            }
        }
    }
}

void WebGLVkContext::uniformMatrix3fv(WebGLUniformLocation loc, GLsizei count, GLboolean /*transpose*/, const GLfloat* value) {
    if (loc.location < 0 || !value || count <= 0) return;
    auto it = programs_.find(currentProgramId_);
    if (it == programs_.end()) return;
    if (const auto* u = findUniform(it->second, loc.location)) {
        for (GLsizei c = 0; c < count; ++c) {
            size_t base = u->offset + c * 48;
            if (base + 44 <= it->second.uniformBytes.size()) {
                const float* src = value + c * 9;
                std::memcpy(it->second.uniformBytes.data() + base + 0, src + 0, 12);
                std::memcpy(it->second.uniformBytes.data() + base + 16, src + 3, 12);
                std::memcpy(it->second.uniformBytes.data() + base + 32, src + 6, 12);
            }
        }
    }
}

void WebGLVkContext::uniformMatrix4fv(WebGLUniformLocation loc, GLsizei count, GLboolean /*transpose*/, const GLfloat* value) {
    if (loc.location < 0 || !value || count <= 0) return;
    auto it = programs_.find(currentProgramId_);
    if (it == programs_.end()) return;
    if (const auto* u = findUniform(it->second, loc.location)) {
        size_t bytes = std::min(static_cast<size_t>(count) * 64, static_cast<size_t>(u->size));
        if (u->offset + bytes <= it->second.uniformBytes.size()) {
            std::memcpy(it->second.uniformBytes.data() + u->offset, value, bytes);
        }
    }
}

void WebGLVkContext::uniformMatrix2x3fv(WebGLUniformLocation loc, GLsizei count, GLboolean /*transpose*/, const GLfloat* value) {
    if (loc.location < 0 || !value || count <= 0) return;
    auto it = programs_.find(currentProgramId_);
    if (it == programs_.end()) return;
    if (const auto* u = findUniform(it->second, loc.location)) {
        for (GLsizei c = 0; c < count; ++c) {
            size_t base = u->offset + c * 32;
            if (base + 28 <= it->second.uniformBytes.size()) {
                const float* src = value + c * 6;
                std::memcpy(it->second.uniformBytes.data() + base + 0, src + 0, 12);
                std::memcpy(it->second.uniformBytes.data() + base + 16, src + 3, 12);
            }
        }
    }
}

void WebGLVkContext::uniformMatrix3x2fv(WebGLUniformLocation loc, GLsizei count, GLboolean /*transpose*/, const GLfloat* value) {
    if (loc.location < 0 || !value || count <= 0) return;
    auto it = programs_.find(currentProgramId_);
    if (it == programs_.end()) return;
    if (const auto* u = findUniform(it->second, loc.location)) {
        for (GLsizei c = 0; c < count; ++c) {
            size_t base = u->offset + c * 48;
            if (base + 40 <= it->second.uniformBytes.size()) {
                const float* src = value + c * 6;
                std::memcpy(it->second.uniformBytes.data() + base + 0, src + 0, 8);
                std::memcpy(it->second.uniformBytes.data() + base + 16, src + 2, 8);
                std::memcpy(it->second.uniformBytes.data() + base + 32, src + 4, 8);
            }
        }
    }
}

// ---------------------------------------------------------------------------
// Program Introspection & Uniform Blocks
// ---------------------------------------------------------------------------

std::vector<GLuint> WebGLVkContext::getUniformIndices(WebGLProgram p, const std::vector<std::string>& names) {
    std::vector<GLuint> res(names.size(), GL_INVALID_INDEX);
    auto it = programs_.find(p.id);
    if (it == programs_.end()) return res;

    for (size_t i = 0; i < names.size(); ++i) {
        const std::string& name = names[i];
        for (size_t u = 0; u < it->second.uniforms.size(); ++u) {
            const auto& uInfo = it->second.uniforms[u];
            if (uInfo.name == name || uInfo.name == (name + "[0]")) {
                res[i] = static_cast<GLuint>(u);
                break;
            }
        }
    }
    return res;
}

std::vector<GLint> WebGLVkContext::getActiveUniforms(WebGLProgram p, const std::vector<GLuint>& indices, GLenum pname) {
    std::vector<GLint> res(indices.size(), 0);
    auto it = programs_.find(p.id);
    if (it == programs_.end()) return res;

    for (size_t i = 0; i < indices.size(); ++i) {
        GLuint idx = indices[i];
        if (idx >= it->second.uniforms.size()) continue;
        const auto& u = it->second.uniforms[idx];
        switch (pname) {
            case GL_UNIFORM_TYPE: res[i] = u.type; break;
            case GL_UNIFORM_SIZE: res[i] = u.count; break;
            case GL_UNIFORM_BLOCK_INDEX: res[i] = u.blockIndex; break;
            case GL_UNIFORM_OFFSET: res[i] = u.offset; break;
            case GL_UNIFORM_ARRAY_STRIDE: res[i] = u.arrayStride; break;
            case GL_UNIFORM_MATRIX_STRIDE: res[i] = u.matrixStride; break;
            case GL_UNIFORM_IS_ROW_MAJOR: res[i] = u.isRowMajor ? 1 : 0; break;
            default: break;
        }
    }
    return res;
}

GLint WebGLVkContext::getActiveUniformBlockParameteri(WebGLProgram p, GLuint blockIndex, GLenum pname) {
    auto it = programs_.find(p.id);
    if (it == programs_.end() || blockIndex >= it->second.uniformBlocks.size()) return 0;
    const auto& b = it->second.uniformBlocks[blockIndex];
    switch (pname) {
        case 0x8A3F /* GL_UNIFORM_BLOCK_BINDING */: return b.binding;
        case GL_UNIFORM_BLOCK_DATA_SIZE: return b.dataSize;
        case GL_UNIFORM_BLOCK_ACTIVE_UNIFORMS: return static_cast<GLint>(b.activeUniformIndices.size());
        case GL_UNIFORM_BLOCK_REFERENCED_BY_VERTEX_SHADER: return b.referencedByVertex ? 1 : 0;
        case GL_UNIFORM_BLOCK_REFERENCED_BY_FRAGMENT_SHADER: return b.referencedByFragment ? 1 : 0;
        default: return 0;
    }
}

std::vector<GLint> WebGLVkContext::getActiveUniformBlockIndices(WebGLProgram p, GLuint blockIndex) {
    auto it = programs_.find(p.id);
    if (it == programs_.end() || blockIndex >= it->second.uniformBlocks.size()) return {};
    const auto& b = it->second.uniformBlocks[blockIndex];
    return std::vector<GLint>(b.activeUniformIndices.begin(), b.activeUniformIndices.end());
}

std::string WebGLVkContext::getActiveUniformBlockName(WebGLProgram p, GLuint blockIndex) {
    auto it = programs_.find(p.id);
    if (it == programs_.end() || blockIndex >= it->second.uniformBlocks.size()) return "";
    return it->second.uniformBlocks[blockIndex].name;
}

} // namespace bro::webgl::vk
