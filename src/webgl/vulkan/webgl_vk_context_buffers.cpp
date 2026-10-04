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
        submitAndFlush();
        if (it->second.allocId != 0) {
            context_.destroyBuffer(it->second.buffer, it->second.allocId);
        } else {
            VkDevice dev = context_.device();
            if (it->second.buffer != VK_NULL_HANDLE) vkDestroyBuffer(dev, it->second.buffer, nullptr);
            if (it->second.memory != VK_NULL_HANDLE) vkFreeMemory(dev, it->second.memory, nullptr);
        }
        buffers_.erase(it);
    }
    if (boundArrayBuffer_ == buf.id) boundArrayBuffer_ = 0;
    if (boundElementArrayBuffer_ == buf.id) boundElementArrayBuffer_ = 0;
    if (boundPixelPackBuffer_ == buf.id) boundPixelPackBuffer_ = 0;
    if (boundPixelUnpackBuffer_ == buf.id) boundPixelUnpackBuffer_ = 0;
    if (boundUniformBuffer_ == buf.id) boundUniformBuffer_ = 0;
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
    if (target == GL_UNIFORM_BUFFER) {
        if (index < boundUniformBuffers_.size()) {
            boundUniformBuffers_[index] = buf.id;
        }
        boundUniformBuffer_ = buf.id;
    } else {
        bindBuffer(target, buf);
    }
}

void WebGLVkContext::bindBufferRange(GLenum target, GLuint index, WebGLBuffer buf, GLintptr /*offset*/, GLsizeiptr /*size*/) {
    bindBufferBase(target, index, buf);
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
    VkDevice dev = context_.device();

    if (res.buffer != VK_NULL_HANDLE) {
        submitAndFlush();
        if (res.allocId != 0) {
            context_.destroyBuffer(res.buffer, res.allocId);
        } else {
            vkDestroyBuffer(dev, res.buffer, nullptr);
            vkFreeMemory(dev, res.memory, nullptr);
        }
        res.buffer = VK_NULL_HANDLE;
        res.memory = VK_NULL_HANDLE;
        res.allocId = 0;
        res.offset = 0;
        res.poolMappedData = nullptr;
    }

    if (size <= 0) {
        res.size = 0;
        res.shadowData.clear();
        return;
    }

    res.size = static_cast<VkDeviceSize>(size);
    res.shadowData.resize(size);
    if (data) {
        std::memcpy(res.shadowData.data(), data, size);
    } else {
        std::memset(res.shadowData.data(), 0, size);
    }

    VkBufferUsageFlags vkUsage = VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    if (target == GL_ARRAY_BUFFER) vkUsage |= VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
    else if (target == GL_ELEMENT_ARRAY_BUFFER) vkUsage |= VK_BUFFER_USAGE_INDEX_BUFFER_BIT;
    else if (target == GL_UNIFORM_BUFFER) vkUsage |= VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;

    if (!context_.createBuffer(res.size, vkUsage,
                               VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                               res.buffer, res.memory, res.offset, res.allocId, res.poolMappedData)) {
        LOG_ERROR("WebGLVkContext: Failed to allocate VkBuffer (%zu bytes)", size);
        setSyntheticError(GL_OUT_OF_MEMORY);
        return;
    }

    if (res.poolMappedData) {
        std::memcpy(res.poolMappedData, res.shadowData.data(), size);
    } else {
        void* mapped = nullptr;
        if (vkMapMemory(dev, res.memory, res.offset, res.size, 0, &mapped) == VK_SUCCESS) {
            std::memcpy(mapped, res.shadowData.data(), size);
            vkUnmapMemory(dev, res.memory);
        } else {
            LOG_ERROR("WebGLVkContext: Failed to map memory for buffer data (%zu bytes)", size);
            setSyntheticError(GL_OUT_OF_MEMORY);
        }
    }
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

    if (res.poolMappedData) {
        std::memcpy(static_cast<char*>(res.poolMappedData) + offset, data, size);
    } else {
        void* mapped = nullptr;
        if (vkMapMemory(context_.device(), res.memory, res.offset + offset, size, 0, &mapped) == VK_SUCCESS) {
            std::memcpy(mapped, data, size);
            vkUnmapMemory(context_.device(), res.memory);
        } else {
            LOG_ERROR("WebGLVkContext: Failed to map memory for bufferSubData (offset %ld, size %zu)", static_cast<long>(offset), static_cast<size_t>(size));
            setSyntheticError(GL_OUT_OF_MEMORY);
        }
    }
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

    std::memcpy(writeRes.shadowData.data() + writeOffset,
                readRes.shadowData.data() + readOffset, size);

    if (writeRes.buffer != VK_NULL_HANDLE) {
        if (writeRes.poolMappedData) {
            std::memcpy(static_cast<char*>(writeRes.poolMappedData) + writeOffset, writeRes.shadowData.data() + writeOffset, size);
        } else {
            void* mapped = nullptr;
            if (vkMapMemory(context_.device(), writeRes.memory, writeRes.offset + writeOffset, size, 0, &mapped) == VK_SUCCESS) {
                std::memcpy(mapped, writeRes.shadowData.data() + writeOffset, size);
                vkUnmapMemory(context_.device(), writeRes.memory);
            } else {
                LOG_ERROR("WebGLVkContext: Failed to map memory for copyBufferSubData");
                setSyntheticError(GL_OUT_OF_MEMORY);
            }
        }
    }
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

    if (res.buffer != VK_NULL_HANDLE && res.memory != VK_NULL_HANDLE) {
        if (res.poolMappedData) {
            std::memcpy(res.poolMappedData, res.shadowData.data(), res.size);
        } else {
            void* mapped = nullptr;
            if (vkMapMemory(context_.device(), res.memory, res.offset, res.size, 0, &mapped) == VK_SUCCESS) {
                std::memcpy(mapped, res.shadowData.data(), res.size);
                vkUnmapMemory(context_.device(), res.memory);
            }
        }
    }

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
    if (res.buffer != VK_NULL_HANDLE && res.memory != VK_NULL_HANDLE && length > 0) {
        if (res.poolMappedData) {
            std::memcpy(static_cast<char*>(res.poolMappedData) + offset, res.shadowData.data() + offset, length);
        } else {
            void* mapped = nullptr;
            if (vkMapMemory(context_.device(), res.memory, res.offset + offset, length, 0, &mapped) == VK_SUCCESS) {
                std::memcpy(mapped, res.shadowData.data() + offset, length);
                vkUnmapMemory(context_.device(), res.memory);
            }
        }
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

void WebGLVkContext::readPixelsToPBO(GLint x, GLint y, GLsizei width, GLsizei height,
                                     GLenum format, GLenum type, GLintptr offset)
{
    if (boundPixelPackBuffer_ == 0) {
        setSyntheticError(GL_INVALID_OPERATION);
        return;
    }
    VkBufferResource& pbo = buffers_[boundPixelPackBuffer_];
    size_t byteCount = static_cast<size_t>(width) * height * 4;
    if (offset < 0 || offset + byteCount > pbo.shadowData.size()) {
        setSyntheticError(GL_INVALID_OPERATION);
        return;
    }

    readPixels(x, y, width, height, format, type, pbo.shadowData.data() + offset);

    if (pbo.poolMappedData) {
        std::memcpy(static_cast<char*>(pbo.poolMappedData) + offset, pbo.shadowData.data() + offset, byteCount);
    } else if (pbo.buffer != VK_NULL_HANDLE && pbo.memory != VK_NULL_HANDLE) {
        void* mapped = nullptr;
        if (vkMapMemory(context_.device(), pbo.memory, pbo.offset + offset, byteCount, 0, &mapped) == VK_SUCCESS) {
            std::memcpy(mapped, pbo.shadowData.data() + offset, byteCount);
            vkUnmapMemory(context_.device(), pbo.memory);
        } else {
            LOG_ERROR("WebGLVkContext: Failed to map memory for readPixelsToPBO");
            setSyntheticError(GL_OUT_OF_MEMORY);
        }
    }
}

} // namespace bro::webgl::vk
