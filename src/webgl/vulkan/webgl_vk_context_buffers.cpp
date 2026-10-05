#include "webgl/vulkan/webgl_vk_context.h"
#include "util/log.h"
#include <algorithm>
#include <cstring>

namespace bro::webgl::vk {

WebGLBuffer WebGLVkContext::createBuffer() {
    GLuint id = nextBufferId_++;
    buffers_[id] = VkBufferResource{};
    return {id};
}

void WebGLVkContext::deleteBuffer(WebGLBuffer buf) {
    auto it = buffers_.find(buf.id);
    if (it != buffers_.end()) {
        releaseBuffer(it->second);
        buffers_.erase(it);
    }
    if (boundArrayBuffer_ == buf.id) boundArrayBuffer_ = 0;
    if (boundElementArrayBuffer_ == buf.id) boundElementArrayBuffer_ = 0;
    if (boundPixelPackBuffer_ == buf.id) boundPixelPackBuffer_ = 0;
    if (boundPixelUnpackBuffer_ == buf.id) boundPixelUnpackBuffer_ = 0;
    if (boundUniformBuffer_ == buf.id) boundUniformBuffer_ = 0;
    for (IndexedBuffer& b : boundUniformBuffers_)
        if (b.buffer == buf.id) b = IndexedBuffer{};
    if (boundCopyReadBuffer_ == buf.id) boundCopyReadBuffer_ = 0;
    if (boundCopyWriteBuffer_ == buf.id) boundCopyWriteBuffer_ = 0;
    if (boundTransformFeedbackBuffer_ == buf.id) boundTransformFeedbackBuffer_ = 0;
}

GLuint WebGLVkContext::getBoundBufferId(GLenum target) const {
    switch (target) {
        case GL_ARRAY_BUFFER: return boundArrayBuffer_;
        case GL_ELEMENT_ARRAY_BUFFER: return boundElementArrayBuffer_;
        case GL_PIXEL_PACK_BUFFER: return boundPixelPackBuffer_;
        case GL_PIXEL_UNPACK_BUFFER: return boundPixelUnpackBuffer_;
        case GL_UNIFORM_BUFFER: return boundUniformBuffer_;
        case GL_COPY_READ_BUFFER: return boundCopyReadBuffer_;
        case GL_COPY_WRITE_BUFFER: return boundCopyWriteBuffer_;
        case GL_TRANSFORM_FEEDBACK_BUFFER: return boundTransformFeedbackBuffer_;
        default: return 0;
    }
}

GLuint WebGLVkContext::boundBuffer(GLenum target) {
    return getBoundBufferId(target);
}

int64_t WebGLVkContext::boundBufferSize(GLenum target) {
    GLuint id = getBoundBufferId(target);
    if (id == 0) return 0;
    auto it = buffers_.find(id);
    return (it != buffers_.end()) ? static_cast<int64_t>(it->second.size) : 0;
}

void WebGLVkContext::bindBuffer(GLenum target, WebGLBuffer buf) {
    switch (target) {
        case GL_ARRAY_BUFFER:
            boundArrayBuffer_ = buf.id;
            break;
        case GL_ELEMENT_ARRAY_BUFFER:
            boundElementArrayBuffer_ = buf.id;
            vaos_[currentVaoId_].elementArrayBufferId = buf.id;
            break;
        case GL_PIXEL_PACK_BUFFER:
            boundPixelPackBuffer_ = buf.id;
            break;
        case GL_PIXEL_UNPACK_BUFFER:
            boundPixelUnpackBuffer_ = buf.id;
            break;
        case GL_UNIFORM_BUFFER:
            boundUniformBuffer_ = buf.id;
            break;
        case GL_COPY_READ_BUFFER:
            boundCopyReadBuffer_ = buf.id;
            break;
        case GL_COPY_WRITE_BUFFER:
            boundCopyWriteBuffer_ = buf.id;
            break;
        case GL_TRANSFORM_FEEDBACK_BUFFER:
            boundTransformFeedbackBuffer_ = buf.id;
            break;
        default:
            setSyntheticError(GL_INVALID_ENUM);
            break;
    }
}

void WebGLVkContext::bindBufferBase(GLenum target, GLuint index, WebGLBuffer buf) {
    if (target != GL_UNIFORM_BUFFER) {
        bindBuffer(target, buf);
        return;
    }
    if (index >= boundUniformBuffers_.size()) {
        setSyntheticError(GL_INVALID_VALUE);
        return;
    }
    boundUniformBuffers_[index] = {buf.id, 0, 0};
    boundUniformBuffer_ = buf.id;
}

// The range is checked against the buffer when a draw uses it (it may be
// bound before the buffer has storage, or the storage may change).
void WebGLVkContext::bindBufferRange(GLenum target, GLuint index, WebGLBuffer buf, GLintptr offset,
                                     GLsizeiptr size) {
    if (target != GL_UNIFORM_BUFFER) {
        bindBuffer(target, buf);
        return;
    }
    if (index >= boundUniformBuffers_.size() || offset < 0 || (buf.id != 0 && size <= 0)) {
        setSyntheticError(GL_INVALID_VALUE);
        return;
    }
    const auto alignment =
        static_cast<GLintptr>(context_.deviceProperties().limits.minUniformBufferOffsetAlignment);
    if (alignment > 0 && offset % alignment != 0) {
        setSyntheticError(GL_INVALID_VALUE);
        return;
    }
    boundUniformBuffers_[index] = {buf.id, buf.id != 0 ? offset : 0, buf.id != 0 ? size : 0};
    boundUniformBuffer_ = buf.id;
}

int64_t WebGLVkContext::getIndexedBufferParameter(GLenum pname, GLuint index) {
    if (index >= boundUniformBuffers_.size()) {
        setSyntheticError(GL_INVALID_VALUE);
        return 0;
    }
    const IndexedBuffer& b = boundUniformBuffers_[index];
    if (pname == GL_UNIFORM_BUFFER_START) return b.offset;
    if (pname == GL_UNIFORM_BUFFER_SIZE) return b.size;
    setSyntheticError(GL_INVALID_ENUM);
    return 0;
}

void WebGLVkContext::bufferData(GLenum target, GLsizeiptr size, const void* data, GLenum /*usage*/) {
    if (size < 0) {
        setSyntheticError(GL_INVALID_VALUE);
        return;
    }
    GLuint bufId = getBoundBufferId(target);
    if (bufId == 0) {
        setSyntheticError(GL_INVALID_OPERATION);
        return;
    }

    VkBufferResource& res = buffers_[bufId];
    const VkDeviceSize newSize = static_cast<VkDeviceSize>(size);
    // Re-specifying at the same size keeps the VkBuffer: the new contents are
    // a copy ordered after the draws that read the old ones. A new size gets a
    // new buffer; the old one lives until the GPU is done with it.
    if (res.isValid() && res.size != newSize) releaseBuffer(res);

    res.size = newSize;
    res.shadowData.assign(static_cast<size_t>(size), 0);
    if (data && size > 0) std::memcpy(res.shadowData.data(), data, static_cast<size_t>(size));
    if (size == 0) return;

    if (!res.isValid()) {
        const VkBufferUsageFlags usage =
            VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
            VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT |
            VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
        void* mapped = nullptr;
        if (!context_.createBuffer(res.size, usage, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                                   res.buffer, res.memory, res.offset, res.allocId, mapped)) {
            LOG_ERROR("WebGLVkContext: Failed to allocate VkBuffer (%zu bytes)", static_cast<size_t>(size));
            setSyntheticError(GL_OUT_OF_MEMORY);
            return;
        }
    }
    uploadToBuffer(res, 0, res.shadowData.data(), res.size);
}

void WebGLVkContext::bufferSubData(GLenum target, GLintptr offset, GLsizeiptr size, const void* data) {
    GLuint bufId = getBoundBufferId(target);
    if (bufId == 0) {
        setSyntheticError(GL_INVALID_OPERATION);
        return;
    }
    if (offset < 0 || size < 0) {
        setSyntheticError(GL_INVALID_VALUE);
        return;
    }
    VkBufferResource& res = buffers_[bufId];
    if (!res.isValid() || offset + size > static_cast<GLintptr>(res.size)) {
        setSyntheticError(GL_INVALID_VALUE);
        return;
    }
    if (!data || size == 0) return;

    std::memcpy(res.shadowData.data() + offset, data, size);
    uploadToBuffer(res, static_cast<VkDeviceSize>(offset), data, static_cast<VkDeviceSize>(size));
}

void WebGLVkContext::getBufferSubData(GLenum target, GLintptr srcByteOffset, void* dstData, GLsizeiptr length) {
    GLuint bufId = getBoundBufferId(target);
    if (bufId == 0) {
        setSyntheticError(GL_INVALID_OPERATION);
        return;
    }
    if (srcByteOffset < 0 || length < 0) {
        setSyntheticError(GL_INVALID_VALUE);
        return;
    }
    VkBufferResource& res = buffers_[bufId];
    if (srcByteOffset + length > static_cast<GLintptr>(res.shadowData.size())) {
        setSyntheticError(GL_INVALID_VALUE);
        return;
    }
    if (dstData && length > 0) {
        std::memcpy(dstData, res.shadowData.data() + srcByteOffset, length);
    }
}

void WebGLVkContext::copyBufferSubData(GLenum readTarget, GLenum writeTarget,
                                       GLintptr readOffset, GLintptr writeOffset, GLsizeiptr size)
{
    GLuint readId = getBoundBufferId(readTarget);
    GLuint writeId = getBoundBufferId(writeTarget);
    if (readId == 0 || writeId == 0) {
        setSyntheticError(GL_INVALID_OPERATION);
        return;
    }
    if (readOffset < 0 || writeOffset < 0 || size < 0) {
        setSyntheticError(GL_INVALID_VALUE);
        return;
    }
    VkBufferResource& readRes = buffers_[readId];
    VkBufferResource& writeRes = buffers_[writeId];

    if (readOffset + size > static_cast<GLintptr>(readRes.shadowData.size()) ||
        writeOffset + size > static_cast<GLintptr>(writeRes.shadowData.size())) {
        setSyntheticError(GL_INVALID_VALUE);
        return;
    }
    if (size == 0) return;

    std::memmove(writeRes.shadowData.data() + writeOffset,
                 readRes.shadowData.data() + readOffset, size);
    uploadToBuffer(writeRes, static_cast<VkDeviceSize>(writeOffset),
                   writeRes.shadowData.data() + writeOffset, static_cast<VkDeviceSize>(size));
}

void* WebGLVkContext::mapBufferRange(GLenum target, GLintptr offset, GLsizeiptr length, GLbitfield access) {
    GLuint bufId = getBoundBufferId(target);
    if (bufId == 0) {
        setSyntheticError(GL_INVALID_OPERATION);
        return nullptr;
    }
    constexpr GLbitfield allMask = GL_MAP_READ_BIT | GL_MAP_WRITE_BIT |
                                   GL_MAP_INVALIDATE_RANGE_BIT | GL_MAP_INVALIDATE_BUFFER_BIT |
                                   GL_MAP_FLUSH_EXPLICIT_BIT | GL_MAP_UNSYNCHRONIZED_BIT;
    if ((access & ~allMask) != 0 || (access & (GL_MAP_READ_BIT | GL_MAP_WRITE_BIT)) == 0) {
        setSyntheticError(GL_INVALID_VALUE);
        return nullptr;
    }
    if (((access & (GL_MAP_INVALIDATE_RANGE_BIT | GL_MAP_FLUSH_EXPLICIT_BIT)) != 0) &&
        !(access & GL_MAP_WRITE_BIT)) {
        setSyntheticError(GL_INVALID_OPERATION);
        return nullptr;
    }
    if (offset < 0 || length <= 0) {
        setSyntheticError(GL_INVALID_VALUE);
        return nullptr;
    }
    VkBufferResource& res = buffers_[bufId];
    if (res.isMapped) {
        setSyntheticError(GL_INVALID_OPERATION);
        return nullptr;
    }
    if (!res.isValid() || offset + length > static_cast<GLintptr>(res.size)) {
        setSyntheticError(GL_INVALID_VALUE);
        return nullptr;
    }

    res.isMapped = true;
    res.mappedPtr = res.shadowData.data() + offset;
    return res.mappedPtr;
}

bool WebGLVkContext::unmapBuffer(GLenum target) {
    GLuint bufId = getBoundBufferId(target);
    if (bufId == 0) {
        setSyntheticError(GL_INVALID_OPERATION);
        return false;
    }
    VkBufferResource& res = buffers_[bufId];
    if (!res.isMapped) {
        setSyntheticError(GL_INVALID_OPERATION);
        return false;
    }

    uploadToBuffer(res, 0, res.shadowData.data(), res.size);

    res.isMapped = false;
    res.mappedPtr = nullptr;
    return true;
}

void WebGLVkContext::flushMappedBufferRange(GLenum target, GLintptr offset, GLsizeiptr length) {
    GLuint bufId = getBoundBufferId(target);
    if (bufId == 0 || !buffers_[bufId].isMapped) {
        setSyntheticError(GL_INVALID_OPERATION);
        return;
    }
    VkBufferResource& res = buffers_[bufId];
    if (offset < 0 || length < 0 || offset + length > static_cast<GLintptr>(res.size)) {
        setSyntheticError(GL_INVALID_VALUE);
        return;
    }
    if (length > 0) {
        uploadToBuffer(res, static_cast<VkDeviceSize>(offset), res.shadowData.data() + offset,
                       static_cast<VkDeviceSize>(length));
    }
}

void WebGLVkContext::texImage2DFromPBO(GLenum target, GLint level, GLint internalformat,
                                       GLsizei width, GLsizei height, GLint border,
                                       GLenum format, GLenum type, GLintptr offset)
{
    if (boundPixelUnpackBuffer_ == 0) {
        setSyntheticError(GL_INVALID_OPERATION);
        return;
    }
    if (unpackFlipY_) {
        setSyntheticError(GL_INVALID_OPERATION);
        return;
    }
    VkBufferResource& pbo = buffers_[boundPixelUnpackBuffer_];
    if (offset < 0 || offset > static_cast<GLintptr>(pbo.shadowData.size())) {
        setSyntheticError(GL_INVALID_OPERATION);
        return;
    }
    const void* ptr = pbo.shadowData.data() + offset;
    texImage2D(target, level, internalformat, width, height, border, format, type, ptr);
}

void WebGLVkContext::texSubImage2DFromPBO(GLenum target, GLint level, GLint xoffset, GLint yoffset,
                                          GLsizei width, GLsizei height,
                                          GLenum format, GLenum type, GLintptr offset)
{
    if (boundPixelUnpackBuffer_ == 0) {
        setSyntheticError(GL_INVALID_OPERATION);
        return;
    }
    if (unpackFlipY_) {
        setSyntheticError(GL_INVALID_OPERATION);
        return;
    }
    VkBufferResource& pbo = buffers_[boundPixelUnpackBuffer_];
    if (offset < 0 || offset > static_cast<GLintptr>(pbo.shadowData.size())) {
        setSyntheticError(GL_INVALID_OPERATION);
        return;
    }
    const void* ptr = pbo.shadowData.data() + offset;
    texSubImage2D(target, level, xoffset, yoffset, width, height, format, type, ptr);
}

} // namespace bro::webgl::vk
