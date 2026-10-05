// Generic vertex attribute values and uniform setters. A program's
// default-block values live in its std140 uniform buffer image
// (VkProgramResource::uniformBytes), uploaded at the next draw that finds it
// changed; sampler uniforms are the texture unit each sampler element reads.

#include "webgl/vulkan/webgl_vk_context.h"
#include "webgl/vulkan/webgl_vk_glsl.h"
#include "util/log.h"

#include <algorithm>
#include <cstring>

namespace bro::webgl::vk {

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

// The uniform a setter's location names in the current program, with the
// GL errors for a location of another program, a missing program, or a
// count for something that is not an array. Null (location -1 included,
// which GL ignores silently) means nothing to write.
VkUniformInfo* WebGLVkContext::uniformTarget(WebGLUniformLocation loc, GLsizei count, VkProgramResource*& prog,
                                             uint32_t& element) {
    if (loc.location < 0) return nullptr;
    auto it = programs_.find(currentProgramId_);
    if (it == programs_.end() || !it->second.linkStatus || loc.program != currentProgramId_) {
        setSyntheticError(GL_INVALID_OPERATION);
        return nullptr;
    }
    if (count < 0) {
        setSyntheticError(GL_INVALID_VALUE);
        return nullptr;
    }
    prog = &it->second;
    if (static_cast<size_t>(loc.location) >= prog->iface.locations.size()) {
        setSyntheticError(GL_INVALID_OPERATION);
        return nullptr;
    }
    const VkUniformLocation& slot = prog->iface.locations[static_cast<size_t>(loc.location)];
    VkUniformInfo& u = prog->iface.uniforms[slot.uniform];
    if (count > 1 && u.size == 1) {
        setSyntheticError(GL_INVALID_OPERATION);
        return nullptr;
    }
    element = slot.element;
    return &u;
}

void WebGLVkContext::setUniformValues(WebGLUniformLocation loc, GLsizei count, UniformKind kind,
                                      uint32_t components, const void* data) {
    VkProgramResource* prog = nullptr;
    uint32_t element = 0;
    VkUniformInfo* u = uniformTarget(loc, count, prog, element);
    if (!u || !data || count == 0) return;
    const glsl::TypeInfo t = glsl::typeInfo(u->type);
    const uint32_t n = std::min(static_cast<uint32_t>(count), static_cast<uint32_t>(u->size) - element);

    if (t.kind == glsl::TypeInfo::Kind::Sampler) {
        if (kind != UniformKind::Int || components != 1) {
            setSyntheticError(GL_INVALID_OPERATION);
            return;
        }
        const auto* units = static_cast<const GLint*>(data);
        const GLint maxUnits = getParameterInt(GL_MAX_COMBINED_TEXTURE_IMAGE_UNITS);
        for (uint32_t i = 0; i < n; ++i) {
            if (units[i] < 0 || units[i] >= maxUnits) {
                setSyntheticError(GL_INVALID_VALUE);
                return;
            }
        }
        const VkSamplerBinding& s = prog->iface.samplers[static_cast<size_t>(u->sampler)];
        for (uint32_t i = 0; i < n; ++i) prog->samplerUnits[s.firstUnit + element + i] = static_cast<uint32_t>(units[i]);
        return;
    }

    const bool kindMatches = t.kind == glsl::TypeInfo::Kind::Bool ||
                             (kind == UniformKind::Float && t.kind == glsl::TypeInfo::Kind::Float) ||
                             (kind == UniformKind::Int && t.kind == glsl::TypeInfo::Kind::Int) ||
                             (kind == UniformKind::Uint && t.kind == glsl::TypeInfo::Kind::Uint);
    if (t.isMatrix() || !kindMatches || t.rows != components) {
        setSyntheticError(GL_INVALID_OPERATION);
        return;
    }
    const bool isBool = t.kind == glsl::TypeInfo::Kind::Bool;
    const auto* words = static_cast<const uint32_t*>(data);
    for (uint32_t i = 0; i < n; ++i) {
        uint8_t* dst = prog->uniformBytes.data() + u->offset + (element + i) * u->arrayStride;
        for (uint32_t c = 0; c < components; ++c) {
            uint32_t word = words[i * components + c];
            // GL's bool is true for any non-zero value; the shader reads 0 / 1.
            if (isBool) {
                float f = 0.0f;
                std::memcpy(&f, &word, 4);
                word = (kind == UniformKind::Float ? f != 0.0f : word != 0) ? 1u : 0u;
            }
            std::memcpy(dst + c * 4, &word, 4);
        }
    }
}

void WebGLVkContext::setUniformMatrices(WebGLUniformLocation loc, GLsizei count, uint32_t columns, uint32_t rows,
                                        GLboolean transpose, const GLfloat* value) {
    VkProgramResource* prog = nullptr;
    uint32_t element = 0;
    VkUniformInfo* u = uniformTarget(loc, count, prog, element);
    if (!u || !value || count == 0) return;
    const glsl::TypeInfo t = glsl::typeInfo(u->type);
    if (!t.isMatrix() || t.columns != columns || t.rows != rows) {
        setSyntheticError(GL_INVALID_OPERATION);
        return;
    }
    const uint32_t n = std::min(static_cast<uint32_t>(count), static_cast<uint32_t>(u->size) - element);
    const uint32_t perMatrix = columns * rows;
    for (uint32_t i = 0; i < n; ++i) {
        const GLfloat* src = value + i * perMatrix;
        uint8_t* dst = prog->uniformBytes.data() + u->offset + (element + i) * u->arrayStride;
        // std140, column-major: each column a 16-byte-aligned vector.
        for (uint32_t c = 0; c < columns; ++c) {
            for (uint32_t r = 0; r < rows; ++r) {
                const GLfloat v = transpose ? src[r * columns + c] : src[c * rows + r];
                std::memcpy(dst + c * u->matrixStride + r * 4, &v, 4);
            }
        }
    }
}

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

void WebGLVkContext::uniform1fv(WebGLUniformLocation l, GLsizei n, const GLfloat* v) {
    setUniformValues(l, n, UniformKind::Float, 1, v);
}
void WebGLVkContext::uniform2fv(WebGLUniformLocation l, GLsizei n, const GLfloat* v) {
    setUniformValues(l, n, UniformKind::Float, 2, v);
}
void WebGLVkContext::uniform3fv(WebGLUniformLocation l, GLsizei n, const GLfloat* v) {
    setUniformValues(l, n, UniformKind::Float, 3, v);
}
void WebGLVkContext::uniform4fv(WebGLUniformLocation l, GLsizei n, const GLfloat* v) {
    setUniformValues(l, n, UniformKind::Float, 4, v);
}
void WebGLVkContext::uniform1iv(WebGLUniformLocation l, GLsizei n, const GLint* v) {
    setUniformValues(l, n, UniformKind::Int, 1, v);
}
void WebGLVkContext::uniform2iv(WebGLUniformLocation l, GLsizei n, const GLint* v) {
    setUniformValues(l, n, UniformKind::Int, 2, v);
}
void WebGLVkContext::uniform3iv(WebGLUniformLocation l, GLsizei n, const GLint* v) {
    setUniformValues(l, n, UniformKind::Int, 3, v);
}
void WebGLVkContext::uniform4iv(WebGLUniformLocation l, GLsizei n, const GLint* v) {
    setUniformValues(l, n, UniformKind::Int, 4, v);
}
void WebGLVkContext::uniform1uiv(WebGLUniformLocation l, GLsizei n, const GLuint* v) {
    setUniformValues(l, n, UniformKind::Uint, 1, v);
}
void WebGLVkContext::uniform2uiv(WebGLUniformLocation l, GLsizei n, const GLuint* v) {
    setUniformValues(l, n, UniformKind::Uint, 2, v);
}
void WebGLVkContext::uniform3uiv(WebGLUniformLocation l, GLsizei n, const GLuint* v) {
    setUniformValues(l, n, UniformKind::Uint, 3, v);
}
void WebGLVkContext::uniform4uiv(WebGLUniformLocation l, GLsizei n, const GLuint* v) {
    setUniformValues(l, n, UniformKind::Uint, 4, v);
}

void WebGLVkContext::uniformMatrix2fv(WebGLUniformLocation l, GLsizei n, GLboolean t, const GLfloat* v) {
    setUniformMatrices(l, n, 2, 2, t, v);
}
void WebGLVkContext::uniformMatrix3fv(WebGLUniformLocation l, GLsizei n, GLboolean t, const GLfloat* v) {
    setUniformMatrices(l, n, 3, 3, t, v);
}
void WebGLVkContext::uniformMatrix4fv(WebGLUniformLocation l, GLsizei n, GLboolean t, const GLfloat* v) {
    setUniformMatrices(l, n, 4, 4, t, v);
}
void WebGLVkContext::uniformMatrix2x3fv(WebGLUniformLocation l, GLsizei n, GLboolean t, const GLfloat* v) {
    setUniformMatrices(l, n, 2, 3, t, v);
}
void WebGLVkContext::uniformMatrix3x2fv(WebGLUniformLocation l, GLsizei n, GLboolean t, const GLfloat* v) {
    setUniformMatrices(l, n, 3, 2, t, v);
}
void WebGLVkContext::uniformMatrix2x4fv(WebGLUniformLocation l, GLsizei n, GLboolean t, const GLfloat* v) {
    setUniformMatrices(l, n, 2, 4, t, v);
}
void WebGLVkContext::uniformMatrix4x2fv(WebGLUniformLocation l, GLsizei n, GLboolean t, const GLfloat* v) {
    setUniformMatrices(l, n, 4, 2, t, v);
}
void WebGLVkContext::uniformMatrix3x4fv(WebGLUniformLocation l, GLsizei n, GLboolean t, const GLfloat* v) {
    setUniformMatrices(l, n, 3, 4, t, v);
}
void WebGLVkContext::uniformMatrix4x3fv(WebGLUniformLocation l, GLsizei n, GLboolean t, const GLfloat* v) {
    setUniformMatrices(l, n, 4, 3, t, v);
}

} // namespace bro::webgl::vk
