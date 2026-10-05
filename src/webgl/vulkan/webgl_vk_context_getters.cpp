// State getters that read what the context holds back out: vertex
// attribute state (getVertexAttrib / getVertexAttribOffset), uniform values
// from a program's std140 image (getUniform), and validateProgram.

#include "webgl/vulkan/webgl_vk_context.h"
#include "webgl/vulkan/webgl_vk_glsl.h"

#include <cstring>

namespace bro::webgl::vk {

bool WebGLVkContext::getVertexAttrib(GLuint index, GLenum pname, GLValue& out) {
    if (index >= 16) {
        setSyntheticError(GL_INVALID_VALUE);
        return false;
    }
    const VkVertexAttribute& a = vaos_[currentVaoId_].attributes[index];
    out = GLValue{};
    auto scalar = [&out](GLValue::Kind kind, uint32_t v) {
        out.kind = kind;
        out.words[0] = v;
        return true;
    };
    switch (pname) {
        case GL_VERTEX_ATTRIB_ARRAY_BUFFER_BINDING: return scalar(GLValue::Kind::Buffer, a.bufferId);
        case GL_VERTEX_ATTRIB_ARRAY_ENABLED: return scalar(GLValue::Kind::Bool, a.enabled ? 1 : 0);
        case GL_VERTEX_ATTRIB_ARRAY_SIZE: return scalar(GLValue::Kind::Int, static_cast<uint32_t>(a.size));
        case GL_VERTEX_ATTRIB_ARRAY_STRIDE: return scalar(GLValue::Kind::Int, static_cast<uint32_t>(a.stride));
        case GL_VERTEX_ATTRIB_ARRAY_TYPE: return scalar(GLValue::Kind::Int, a.type);
        case GL_VERTEX_ATTRIB_ARRAY_NORMALIZED: return scalar(GLValue::Kind::Bool, a.normalized ? 1 : 0);
        case GL_VERTEX_ATTRIB_ARRAY_INTEGER: return scalar(GLValue::Kind::Bool, a.isInteger ? 1 : 0);
        case GL_VERTEX_ATTRIB_ARRAY_DIVISOR: return scalar(GLValue::Kind::Int, a.divisor);
        case GL_CURRENT_VERTEX_ATTRIB: {
            // Typed as the last vertexAttrib* call wrote it.
            switch (genericAttribKinds_[index]) {
                case VkVertexInput::Kind::Int: out.kind = GLValue::Kind::Int; break;
                case VkVertexInput::Kind::Uint: out.kind = GLValue::Kind::Uint; break;
                default: out.kind = GLValue::Kind::Float; break;
            }
            out.array = true;
            out.count = 4;
            std::memcpy(out.words, genericAttribs_[index].data(), 16);
            return true;
        }
        default:
            setSyntheticError(GL_INVALID_ENUM);
            return false;
    }
}

GLintptr WebGLVkContext::getVertexAttribOffset(GLuint index, GLenum pname) {
    if (pname != GL_VERTEX_ATTRIB_ARRAY_POINTER) {
        setSyntheticError(GL_INVALID_ENUM);
        return 0;
    }
    if (index >= 16) {
        setSyntheticError(GL_INVALID_VALUE);
        return 0;
    }
    return static_cast<GLintptr>(vaos_[currentVaoId_].attributes[index].offset);
}

bool WebGLVkContext::getUniform(WebGLProgram p, WebGLUniformLocation loc, GLValue& out) {
    auto it = programs_.find(p.id);
    if (it == programs_.end()) {
        setSyntheticError(GL_INVALID_VALUE);
        return false;
    }
    const VkProgramResource& prog = it->second;
    if (!prog.linkStatus || loc.program != p.id || loc.location < 0 ||
        static_cast<size_t>(loc.location) >= prog.iface.locations.size()) {
        setSyntheticError(GL_INVALID_OPERATION);
        return false;
    }
    const VkUniformLocation& slot = prog.iface.locations[static_cast<size_t>(loc.location)];
    const VkUniformInfo& u = prog.iface.uniforms[slot.uniform];
    const glsl::TypeInfo t = glsl::typeInfo(u.type);
    out = GLValue{};

    if (t.kind == glsl::TypeInfo::Kind::Sampler) {
        const VkSamplerBinding& s = prog.iface.samplers[static_cast<size_t>(u.sampler)];
        out.kind = GLValue::Kind::Int;
        out.words[0] = prog.samplerUnits[s.firstUnit + slot.element];
        return true;
    }
    switch (t.kind) {
        case glsl::TypeInfo::Kind::Int: out.kind = GLValue::Kind::Int; break;
        case glsl::TypeInfo::Kind::Uint: out.kind = GLValue::Kind::Uint; break;
        case glsl::TypeInfo::Kind::Bool: out.kind = GLValue::Kind::Bool; break;
        default: out.kind = GLValue::Kind::Float; break;
    }
    out.count = t.components();
    out.array = out.count > 1;
    // std140: element `slot.element` at arrayStride, a matrix column-major
    // with each column at matrixStride; GL answers column-major too.
    const uint8_t* src = prog.uniformBytes.data() + u.offset + slot.element * u.arrayStride;
    for (uint32_t c = 0; c < t.columns; ++c) {
        for (uint32_t r = 0; r < t.rows; ++r) {
            std::memcpy(&out.words[c * t.rows + r], src + c * u.matrixStride + r * 4, 4);
        }
    }
    return true;
}

// WebGL 2 / ES 3.0 2.11.9: samplers of different types may not read the
// same texture unit; a draw with such a program is INVALID_OPERATION.
bool WebGLVkContext::samplerUnitConflict(const VkProgramResource& prog) const {
    const std::vector<VkSamplerBinding>& samplers = prog.iface.samplers;
    for (size_t i = 0; i < samplers.size(); ++i) {
        for (size_t j = i + 1; j < samplers.size(); ++j) {
            if (samplers[i].type == samplers[j].type) continue;
            for (uint32_t a = 0; a < samplers[i].count; ++a) {
                for (uint32_t b = 0; b < samplers[j].count; ++b) {
                    if (prog.samplerUnits[samplers[i].firstUnit + a] == prog.samplerUnits[samplers[j].firstUnit + b])
                        return true;
                }
            }
        }
    }
    return false;
}

void WebGLVkContext::validateProgram(WebGLProgram p) {
    auto it = programs_.find(p.id);
    if (it == programs_.end()) {
        setSyntheticError(GL_INVALID_VALUE);
        return;
    }
    VkProgramResource& prog = it->second;
    prog.validateStatus = prog.linkStatus && !samplerUnitConflict(prog);
    if (prog.linkStatus && !prog.validateStatus)
        prog.infoLog = "ERROR: samplers of different types read the same texture unit\n";
}

} // namespace bro::webgl::vk
