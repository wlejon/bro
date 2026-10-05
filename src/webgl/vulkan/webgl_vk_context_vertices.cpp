#include "webgl/vulkan/webgl_vk_context.h"
#include "util/log.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace bro::webgl::vk {

namespace {

constexpr GLenum kInt2101010Rev = 0x8D9F;
constexpr GLenum kUnsignedInt2101010Rev = 0x8368;

bool packedType(GLenum type) { return type == kInt2101010Rev || type == kUnsignedInt2101010Rev; }

uint32_t vertexTypeBytes(GLenum type) {
    switch (type) {
        case GL_BYTE: case GL_UNSIGNED_BYTE: return 1;
        case GL_SHORT: case GL_UNSIGNED_SHORT: case GL_HALF_FLOAT: return 2;
        default: return 4;
    }
}

// Bytes one element of the array occupies (and its stride when tightly packed).
uint32_t elementBytes(const VkVertexAttribute& attr) {
    return packedType(attr.type) ? 4 : vertexTypeBytes(attr.type) * static_cast<uint32_t>(attr.size);
}

float halfToFloat(uint16_t h) {
    const uint32_t sign = (h & 0x8000u) << 16, exp = (h >> 10) & 0x1F, mant = h & 0x3FF;
    uint32_t bits;
    if (exp == 0) {
        if (mant == 0) {
            bits = sign;
        } else {  // subnormal: renormalise
            int e = -1;
            uint32_t m = mant;
            do { ++e; m <<= 1; } while (!(m & 0x400));
            bits = sign | static_cast<uint32_t>(127 - 15 - e) << 23 | (m & 0x3FF) << 13;
        }
    } else if (exp == 31) {
        bits = sign | 0x7F800000u | mant << 13;
    } else {
        bits = sign | (exp + 127 - 15) << 23 | mant << 13;
    }
    float f;
    std::memcpy(&f, &bits, 4);
    return f;
}

// One element of a vertex array as four 32-bit components, the way the
// vertex shader reads it: integers as they are for vertexAttribIPointer,
// else float (normalized by ES 3.0's 2.1.6 rules when asked).
void decodeElement(const VkVertexAttribute& attr, const uint8_t* src, uint32_t out[4]) {
    if (packedType(attr.type)) {
        uint32_t v;
        std::memcpy(&v, src, 4);
        const bool sign = attr.type == kInt2101010Rev;
        float c[4];
        for (int i = 0; i < 4; ++i) {
            const uint32_t bits = i < 3 ? 10 : 2;
            const uint32_t shift = i * 10;
            uint32_t raw = (v >> shift) & ((1u << bits) - 1);
            if (sign) {
                const int32_t s = static_cast<int32_t>(raw << (32 - bits)) >> (32 - bits);
                c[i] = attr.normalized ? std::max(static_cast<float>(s) / static_cast<float>((1 << (bits - 1)) - 1), -1.0f)
                                       : static_cast<float>(s);
            } else {
                c[i] = attr.normalized ? static_cast<float>(raw) / static_cast<float>((1u << bits) - 1)
                                       : static_cast<float>(raw);
            }
        }
        std::memcpy(out, c, 16);
        return;
    }
    for (int i = 0; i < 4; ++i) {
        if (i >= attr.size) {
            // Missing components read as (0, 0, 0, 1).
            const float f = i == 3 ? 1.0f : 0.0f;
            if (attr.isInteger) out[i] = i == 3 ? 1u : 0u;
            else std::memcpy(&out[i], &f, 4);
            continue;
        }
        const uint8_t* p = src + i * vertexTypeBytes(attr.type);
        double value = 0.0, scale = 1.0;
        bool isSigned = false;
        switch (attr.type) {
            case GL_BYTE: { int8_t v; std::memcpy(&v, p, 1); value = v; scale = 127.0; isSigned = true; break; }
            case GL_UNSIGNED_BYTE: value = *p; scale = 255.0; break;
            case GL_SHORT: { int16_t v; std::memcpy(&v, p, 2); value = v; scale = 32767.0; isSigned = true; break; }
            case GL_UNSIGNED_SHORT: { uint16_t v; std::memcpy(&v, p, 2); value = v; scale = 65535.0; break; }
            case GL_INT: { int32_t v; std::memcpy(&v, p, 4); value = v; scale = 2147483647.0; isSigned = true; break; }
            case GL_UNSIGNED_INT: { uint32_t v; std::memcpy(&v, p, 4); value = v; scale = 4294967295.0; break; }
            case GL_HALF_FLOAT: { uint16_t v; std::memcpy(&v, p, 2); value = halfToFloat(v); break; }
            default: { float v; std::memcpy(&v, p, 4); value = v; break; }
        }
        if (attr.isInteger) {
            out[i] = isSigned ? static_cast<uint32_t>(static_cast<int32_t>(value)) : static_cast<uint32_t>(value);
            continue;
        }
        const bool fixedPoint = attr.type != GL_FLOAT && attr.type != GL_HALF_FLOAT;
        if (attr.normalized && fixedPoint) value = isSigned ? std::max(value / scale, -1.0) : value / scale;
        const float f = static_cast<float>(value);
        std::memcpy(&out[i], &f, 4);
    }
}

VkFormat decodedFormat(const VkVertexAttribute& attr) {
    const uint32_t n = packedType(attr.type) ? 4 : static_cast<uint32_t>(attr.size);
    const VkFormat floats[] = {VK_FORMAT_R32_SFLOAT, VK_FORMAT_R32G32_SFLOAT, VK_FORMAT_R32G32B32_SFLOAT,
                               VK_FORMAT_R32G32B32A32_SFLOAT};
    const VkFormat sints[] = {VK_FORMAT_R32_SINT, VK_FORMAT_R32G32_SINT, VK_FORMAT_R32G32B32_SINT,
                              VK_FORMAT_R32G32B32A32_SINT};
    const VkFormat uints[] = {VK_FORMAT_R32_UINT, VK_FORMAT_R32G32_UINT, VK_FORMAT_R32G32B32_UINT,
                              VK_FORMAT_R32G32B32A32_UINT};
    if (!attr.isInteger) return floats[n - 1];
    const bool isSigned = attr.type == GL_BYTE || attr.type == GL_SHORT || attr.type == GL_INT;
    return isSigned ? sints[n - 1] : uints[n - 1];
}

VkVertexInput::Kind arrayKind(const VkVertexAttribute& attr) {
    if (!attr.isInteger) return VkVertexInput::Kind::Float;
    const bool isSigned = attr.type == GL_BYTE || attr.type == GL_SHORT || attr.type == GL_INT;
    return isSigned ? VkVertexInput::Kind::Int : VkVertexInput::Kind::Uint;
}

} // namespace

bool WebGLVkContext::vertexFormatSupported(VkFormat format) {
    if (format == VK_FORMAT_UNDEFINED) return false;
    auto it = vertexFormatSupport_.find(format);
    if (it != vertexFormatSupport_.end()) return it->second;
    VkFormatProperties props{};
    vkGetPhysicalDeviceFormatProperties(context_.physicalDevice(), format, &props);
    const bool ok = (props.bufferFeatures & VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT) != 0;
    vertexFormatSupport_.emplace(format, ok);
    return ok;
}

bool WebGLVkContext::bindVertexInputs(const VkProgramResource& prog, const DrawShape& shape, PipelineKey& key,
                                      std::vector<VkBuffer>& vbos, std::vector<VkDeviceSize>& offsets) {
    VkVAOResource& vao = vaos_[currentVaoId_];
    uint32_t count = 0;
    for (const VkVertexInput& in : prog.iface.vertexInputs) {
        const uint32_t loc = in.location;
        if (loc >= vao.attributes.size()) continue;
        const VkVertexAttribute& attr = vao.attributes[loc];
        if (!attr.enabled) {
            // A disabled array reads the attribute's constant value: a zero
            // stride binding over the segment's copy of the generic values,
            // which must be of the kind the shader declares (WebGL 2 5.11).
            if (genericAttribKinds_[loc] != in.kind) {
                LOG_ERROR("WebGLVkContext: vertex attribute %u's value is not of the shader input's type", loc);
                setSyntheticError(GL_INVALID_OPERATION);
                return false;
            }
            if (genericAttribSerial_ != stream_.segmentSerial() || !genericAttribSlice_) {
                genericAttribSlice_ = stage(genericAttribs_.data(), sizeof(genericAttribs_));
                genericAttribSerial_ = stream_.segmentSerial();
            }
            if (!genericAttribSlice_) return false;
            VkFormat format = VK_FORMAT_R32G32B32A32_SFLOAT;
            if (in.kind == VkVertexInput::Kind::Int) format = VK_FORMAT_R32G32B32A32_SINT;
            if (in.kind == VkVertexInput::Kind::Uint) format = VK_FORMAT_R32G32B32A32_UINT;
            key.attributes[count] = {loc, count, format, 0};
            key.bindings[count] = {count, 0, VK_VERTEX_INPUT_RATE_VERTEX};
            vbos.push_back(genericAttribSlice_.buffer);
            offsets.push_back(genericAttribSlice_.offset + loc * 16);
            count++;
            continue;
        }

        auto bIt = attr.bufferId != 0 ? buffers_.find(attr.bufferId) : buffers_.end();
        if (bIt == buffers_.end()) {
            LOG_ERROR("WebGLVkContext: vertex attribute %u is enabled with no buffer", loc);
            setSyntheticError(GL_INVALID_OPERATION);
            return false;
        }
        if (arrayKind(attr) != in.kind) {
            LOG_ERROR("WebGLVkContext: vertex attribute %u's array is not of the shader input's type", loc);
            setSyntheticError(GL_INVALID_OPERATION);
            return false;
        }
        VkBufferResource& buf = bIt->second;

        // The elements the draw fetches: per vertex, or one per `divisor`
        // instances.
        const uint32_t divisor = attr.divisor;
        const uint32_t first = divisor == 0 ? shape.first : 0;
        const uint32_t end = divisor == 0 ? shape.end : (shape.instances + divisor - 1) / divisor;
        const uint32_t elemBytes = elementBytes(attr);
        const uint64_t stride = attr.stride != 0 ? static_cast<uint64_t>(attr.stride) : elemBytes;
        if (end > first &&
            attr.offset + (end - 1) * stride + elemBytes > static_cast<uint64_t>(buf.shadowData.size())) {
            LOG_ERROR("WebGLVkContext: vertex attribute %u reads past the end of its buffer", loc);
            setSyntheticError(GL_INVALID_OPERATION);
            return false;
        }

        const VkFormat format = glTypeToVkFormat(attr.type, attr.size, attr.normalized, attr.isInteger);
        const bool divisorNative = divisor <= 1 || context_.vertexAttributeDivisor();
        if (vertexFormatSupported(format) && divisorNative && buf.isValid()) {
            key.attributes[count] = {loc, count, format, 0};
            key.bindings[count] = {count, static_cast<uint32_t>(stride),
                                   divisor > 0 ? VK_VERTEX_INPUT_RATE_INSTANCE : VK_VERTEX_INPUT_RATE_VERTEX};
            if (divisor > 1) key.divisors[count] = divisor;
            vbos.push_back(buf.buffer);
            offsets.push_back(attr.offset);
            count++;
            continue;
        }

        // A format the device cannot fetch, or a divisor it cannot step by:
        // decode the fetched elements into 32-bit components in the
        // segment's upload memory, one per vertex (or per instance), at the
        // index the draw fetches them by.
        const VkFormat decoded = decodedFormat(attr);
        const uint32_t components = packedType(attr.type) ? 4 : static_cast<uint32_t>(attr.size);
        const uint32_t outStride = components * 4;
        const uint32_t outEnd = divisor > 1 ? shape.instances : end;
        const uint32_t outFirst = divisor > 0 ? 0 : first;
        if (!syncShadow(buf)) return false;
        render::UploadSlice slice = stage(nullptr, std::max<VkDeviceSize>(outEnd, 1) * outStride, 4);
        if (!slice) return false;
        auto* out = static_cast<uint8_t*>(slice.mapped);
        for (uint32_t i = outFirst; i < outEnd; ++i) {
            const uint32_t element = divisor > 1 ? i / divisor : i;
            uint32_t v[4];
            decodeElement(attr, buf.shadowData.data() + attr.offset + element * stride, v);
            std::memcpy(out + static_cast<size_t>(i) * outStride, v, outStride);
        }
        key.attributes[count] = {loc, count, decoded, 0};
        key.bindings[count] = {count, outStride,
                               divisor > 0 ? VK_VERTEX_INPUT_RATE_INSTANCE : VK_VERTEX_INPUT_RATE_VERTEX};
        vbos.push_back(slice.buffer);
        offsets.push_back(slice.offset);
        count++;
    }
    key.attributeCount = count;
    key.bindingCount = count;
    return true;
}

// ---------------------------------------------------------------------------
// Vertex array state
// ---------------------------------------------------------------------------

void WebGLVkContext::enableVertexAttribArray(GLuint index) {
    if (index >= 16) {
        setSyntheticError(GL_INVALID_VALUE);
        return;
    }
    vaos_[currentVaoId_].attributes[index].enabled = true;
}

void WebGLVkContext::disableVertexAttribArray(GLuint index) {
    if (index >= 16) {
        setSyntheticError(GL_INVALID_VALUE);
        return;
    }
    vaos_[currentVaoId_].attributes[index].enabled = false;
}

namespace {

// The ES 3.0 2.9 errors of glVertexAttrib[I]Pointer, or NO_ERROR.
GLenum pointerError(GLuint index, GLint size, GLenum type, GLsizei stride, uintptr_t offset, bool integer,
                    bool haveBuffer) {
    if (index >= 16 || stride < 0 || stride > 255) return GL_INVALID_VALUE;
    switch (type) {
        case GL_BYTE: case GL_UNSIGNED_BYTE: case GL_SHORT: case GL_UNSIGNED_SHORT: case GL_INT:
        case GL_UNSIGNED_INT:
            break;
        case GL_FLOAT: case GL_HALF_FLOAT: case kInt2101010Rev: case kUnsignedInt2101010Rev:
            if (integer) return GL_INVALID_ENUM;
            break;
        default: return GL_INVALID_ENUM;
    }
    if (size < 1 || size > 4) return GL_INVALID_VALUE;
    if (packedType(type) && size != 4) return GL_INVALID_OPERATION;
    // WebGL 6.4: offsets and strides are multiples of the type's size.
    const uint32_t bytes = vertexTypeBytes(type);
    if (offset % bytes != 0 || static_cast<uint32_t>(stride) % bytes != 0) return GL_INVALID_OPERATION;
    // WebGL 6.6: a non-zero offset needs a buffer.
    if (!haveBuffer && offset != 0) return GL_INVALID_OPERATION;
    return GL_NO_ERROR;
}

} // namespace

void WebGLVkContext::vertexAttribPointer(GLuint index, GLint size, GLenum type, GLboolean normalized,
                                         GLsizei stride, uintptr_t offset) {
    const GLenum err = pointerError(index, size, type, stride, offset, false, boundArrayBuffer_ != 0);
    if (err != GL_NO_ERROR) {
        setSyntheticError(err);
        return;
    }
    VkVertexAttribute& attr = vaos_[currentVaoId_].attributes[index];
    attr.size = size;
    attr.type = type;
    attr.normalized = normalized;
    attr.isInteger = false;
    attr.stride = stride;
    attr.offset = offset;
    attr.bufferId = boundArrayBuffer_;
}

void WebGLVkContext::vertexAttribIPointer(GLuint index, GLint size, GLenum type, GLsizei stride,
                                          uintptr_t offset) {
    const GLenum err = pointerError(index, size, type, stride, offset, true, boundArrayBuffer_ != 0);
    if (err != GL_NO_ERROR) {
        setSyntheticError(err);
        return;
    }
    VkVertexAttribute& attr = vaos_[currentVaoId_].attributes[index];
    attr.size = size;
    attr.type = type;
    attr.normalized = GL_FALSE;
    attr.isInteger = true;
    attr.stride = stride;
    attr.offset = offset;
    attr.bufferId = boundArrayBuffer_;
}

void WebGLVkContext::vertexAttribDivisor(GLuint index, GLuint divisor) {
    if (index >= 16) {
        setSyntheticError(GL_INVALID_VALUE);
        return;
    }
    vaos_[currentVaoId_].attributes[index].divisor = divisor;
}

} // namespace bro::webgl::vk
