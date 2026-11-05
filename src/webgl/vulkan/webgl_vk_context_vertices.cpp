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
    const auto idx = static_cast<uint32_t>(format);
    if (idx < vertexFormatSupport_.size()) {
        uint8_t s = vertexFormatSupport_[idx];
        if (s != 0) return s == 2;
        VkFormatProperties props{};
        vkGetPhysicalDeviceFormatProperties(context_.physicalDevice(), format, &props);
        const bool ok = (props.bufferFeatures & VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT) != 0;
        vertexFormatSupport_[idx] = ok ? 2 : 1;
        return ok;
    }
    VkFormatProperties props{};
    vkGetPhysicalDeviceFormatProperties(context_.physicalDevice(), format, &props);
    return (props.bufferFeatures & VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT) != 0;
}

// How each of the program's inputs is fed from the current VAO (a constant,
// the array's own buffer, or decoded elements) and the vertex input state
// that makes; equal layouts share an id, so pipelines remembered per program
// hold across VAOs that read the same way.
void WebGLVkContext::buildVertexLayout(const VkProgramResource& prog, VkVAOResource& vao, uint32_t validMask) {
    VaoInputCache& c = vao.inputs;
    c = VaoInputCache{};
    VertexLayout& layout = c.layout;
    uint32_t count = 0;
    const auto& inputs = prog.iface.vertexInputs;
    for (size_t i = 0; i < inputs.size() && i < c.paths.size(); ++i) {
        const VkVertexInput& in = inputs[i];
        const uint32_t loc = in.location;
        if (loc >= vao.attributes.size()) continue;  // Skip
        const VkVertexAttribute& attr = vao.attributes[loc];
        if (!attr.enabled) {
            VkFormat format = VK_FORMAT_R32G32B32A32_SFLOAT;
            if (in.kind == VkVertexInput::Kind::Int) format = VK_FORMAT_R32G32B32A32_SINT;
            if (in.kind == VkVertexInput::Kind::Uint) format = VK_FORMAT_R32G32B32A32_UINT;
            layout.attributes[count] = {loc, count, format, 0};
            layout.bindings[count] = {count, 0, VK_VERTEX_INPUT_RATE_VERTEX};
            c.paths[i] = VaoInputCache::Path::Generic;
            count++;
            continue;
        }
        const uint32_t divisor = attr.divisor;
        const VkVertexInputRate rate = divisor > 0 ? VK_VERTEX_INPUT_RATE_INSTANCE : VK_VERTEX_INPUT_RATE_VERTEX;
        const VkFormat format = glTypeToVkFormat(attr.type, attr.size, attr.normalized, attr.isInteger);
        const bool divisorNative = divisor <= 1 || context_.vertexAttributeDivisor();
        if (vertexFormatSupported(format) && divisorNative && (validMask & (1u << i))) {
            const uint32_t stride = attr.stride != 0 ? static_cast<uint32_t>(attr.stride) : elementBytes(attr);
            layout.attributes[count] = {loc, count, format, 0};
            layout.bindings[count] = {count, stride, rate};
            if (divisor > 1) layout.divisors[count] = divisor;
            c.paths[i] = VaoInputCache::Path::Direct;
        } else {
            // A format the device cannot fetch, or a divisor it cannot step
            // by: the draw decodes the elements into 32-bit components.
            const uint32_t components = packedType(attr.type) ? 4 : static_cast<uint32_t>(attr.size);
            layout.attributes[count] = {loc, count, decodedFormat(attr), 0};
            layout.bindings[count] = {count, components * 4, rate};
            c.paths[i] = VaoInputCache::Path::Decode;
        }
        count++;
    }
    layout.count = count;
    c.linkSerial = prog.linkSerial;
    c.version = vao.version;
    c.validMask = validMask;

    for (const auto& [known, id] : vertexLayouts_) {
        if (known == layout) {
            c.layoutId = id;
            return;
        }
    }
    if (vertexLayouts_.size() >= 256) vertexLayouts_.clear();  // ids stay unique; only sharing is lost
    c.layoutId = nextLayoutId_++;
    vertexLayouts_.emplace_back(layout, c.layoutId);
}

const VaoInputCache* WebGLVkContext::bindVertexInputs(const VkProgramResource& prog, const DrawShape& shape,
                                                      std::vector<VkBuffer>& vbos,
                                                      std::vector<VkDeviceSize>& offsets) {
    VkVAOResource& vao = *currentVao_;
    const auto& inputs = prog.iface.vertexInputs;
    const size_t n = std::min(inputs.size(), vao.inputs.paths.size());

    // What the draw reads must be there, of the shader's kinds, and long
    // enough for the elements it fetches.
    std::array<VkBufferResource*, 16> bufs{};
    uint32_t validMask = 0;
    for (size_t i = 0; i < n; ++i) {
        const VkVertexInput& in = inputs[i];
        const uint32_t loc = in.location;
        if (loc >= vao.attributes.size()) continue;
        const VkVertexAttribute& attr = vao.attributes[loc];
        if (!attr.enabled) {
            // A disabled array reads the attribute's constant value, which
            // must be of the kind the shader declares (WebGL 2 5.11).
            if (genericAttribKinds_[loc] != in.kind) {
                LOG_ERROR("WebGLVkContext: vertex attribute %u's value is not of the shader input's type", loc);
                setSyntheticError(GL_INVALID_OPERATION);
                return nullptr;
            }
            continue;
        }
        auto bIt = attr.bufferId != 0 ? buffers_.find(attr.bufferId) : buffers_.end();
        if (bIt == buffers_.end()) {
            LOG_ERROR("WebGLVkContext: vertex attribute %u is enabled with no buffer", loc);
            setSyntheticError(GL_INVALID_OPERATION);
            return nullptr;
        }
        if (arrayKind(attr) != in.kind) {
            LOG_ERROR("WebGLVkContext: vertex attribute %u's array is not of the shader input's type", loc);
            setSyntheticError(GL_INVALID_OPERATION);
            return nullptr;
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
            return nullptr;
        }
        bufs[i] = &buf;
        if (buf.isValid()) validMask |= 1u << i;
    }

    const VaoInputCache& c = vao.inputs;
    if (c.linkSerial != prog.linkSerial || c.version != vao.version || c.validMask != validMask)
        buildVertexLayout(prog, vao, validMask);

    for (size_t i = 0; i < n; ++i) {
        switch (c.paths[i]) {
            case VaoInputCache::Path::Skip: break;
            case VaoInputCache::Path::Generic: {
                // A zero stride binding over the segment's copy of the
                // generic values.
                if (genericAttribSerial_ != stream_.segmentSerial() || !genericAttribSlice_) {
                    genericAttribSlice_ = stage(genericAttribs_.data(), sizeof(genericAttribs_));
                    genericAttribSerial_ = stream_.segmentSerial();
                }
                if (!genericAttribSlice_) return nullptr;
                vbos.push_back(genericAttribSlice_.buffer);
                offsets.push_back(genericAttribSlice_.offset + inputs[i].location * 16);
                break;
            }
            case VaoInputCache::Path::Direct:
                vbos.push_back(bufs[i]->buffer);
                offsets.push_back(vao.attributes[inputs[i].location].offset);
                break;
            case VaoInputCache::Path::Decode: {
                // Decode the fetched elements into 32-bit components in the
                // segment's upload memory, one per vertex (or per instance),
                // at the index the draw fetches them by.
                const VkVertexAttribute& attr = vao.attributes[inputs[i].location];
                VkBufferResource& buf = *bufs[i];
                const uint32_t divisor = attr.divisor;
                const uint32_t end = divisor == 0 ? shape.end : (shape.instances + divisor - 1) / divisor;
                const uint64_t stride = attr.stride != 0 ? static_cast<uint64_t>(attr.stride) : elementBytes(attr);
                const uint32_t outStride = c.layout.bindings[vbos.size()].stride;
                const uint32_t outEnd = divisor > 1 ? shape.instances : end;
                const uint32_t outFirst = divisor > 0 ? 0 : shape.first;
                if (!syncShadow(buf)) return nullptr;
                render::UploadSlice slice = stage(nullptr, std::max<VkDeviceSize>(outEnd, 1) * outStride, 4);
                if (!slice) return nullptr;
                auto* out = static_cast<uint8_t*>(slice.mapped);
                for (uint32_t e = outFirst; e < outEnd; ++e) {
                    const uint32_t element = divisor > 1 ? e / divisor : e;
                    uint32_t v[4];
                    decodeElement(attr, buf.shadowData.data() + attr.offset + element * stride, v);
                    std::memcpy(out + static_cast<size_t>(e) * outStride, v, outStride);
                }
                vbos.push_back(slice.buffer);
                offsets.push_back(slice.offset);
                break;
            }
        }
    }
    return &c;
}

// ---------------------------------------------------------------------------
// Vertex array state
// ---------------------------------------------------------------------------

void WebGLVkContext::enableVertexAttribArray(GLuint index) {
    if (index >= 16) {
        setSyntheticError(GL_INVALID_VALUE);
        return;
    }
    currentVao_->attributes[index].enabled = true;
    ++currentVao_->version;
}

void WebGLVkContext::disableVertexAttribArray(GLuint index) {
    if (index >= 16) {
        setSyntheticError(GL_INVALID_VALUE);
        return;
    }
    currentVao_->attributes[index].enabled = false;
    ++currentVao_->version;
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
    VkVertexAttribute& attr = currentVao_->attributes[index];
    attr.size = size;
    attr.type = type;
    attr.normalized = normalized;
    attr.isInteger = false;
    attr.stride = stride;
    attr.offset = offset;
    attr.bufferId = boundArrayBuffer_;
    ++currentVao_->version;
}

void WebGLVkContext::vertexAttribIPointer(GLuint index, GLint size, GLenum type, GLsizei stride,
                                          uintptr_t offset) {
    const GLenum err = pointerError(index, size, type, stride, offset, true, boundArrayBuffer_ != 0);
    if (err != GL_NO_ERROR) {
        setSyntheticError(err);
        return;
    }
    VkVertexAttribute& attr = currentVao_->attributes[index];
    attr.size = size;
    attr.type = type;
    attr.normalized = GL_FALSE;
    attr.isInteger = true;
    attr.stride = stride;
    attr.offset = offset;
    attr.bufferId = boundArrayBuffer_;
    ++currentVao_->version;
}

void WebGLVkContext::vertexAttribDivisor(GLuint index, GLuint divisor) {
    if (index >= 16) {
        setSyntheticError(GL_INVALID_VALUE);
        return;
    }
    currentVao_->attributes[index].divisor = divisor;
    ++currentVao_->version;
}

} // namespace bro::webgl::vk
