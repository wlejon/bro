#include "webgl/vulkan/webgl_vk_context.h"
#include "util/log.h"

#include <algorithm>
#include <cstring>

namespace bro::webgl::vk {

void WebGLVkContext::updateTextureSampler(VkTextureResource& tex) {
    if (tex.sampler != VK_NULL_HANDLE && !tex.samplerDirty) return;
    VkDevice dev = context_.device();
    if (tex.sampler != VK_NULL_HANDLE) {
        vkDestroySampler(dev, tex.sampler, nullptr);
        tex.sampler = VK_NULL_HANDLE;
    }
    VkSamplerCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    info.magFilter = (tex.magFilter == GL_NEAREST) ? VK_FILTER_NEAREST : VK_FILTER_LINEAR;
    info.minFilter = (tex.minFilter == GL_NEAREST || tex.minFilter == GL_NEAREST_MIPMAP_NEAREST) ? VK_FILTER_NEAREST : VK_FILTER_LINEAR;
    info.mipmapMode = (tex.minFilter == GL_LINEAR_MIPMAP_LINEAR || tex.minFilter == GL_NEAREST_MIPMAP_LINEAR) ? VK_SAMPLER_MIPMAP_MODE_LINEAR : VK_SAMPLER_MIPMAP_MODE_NEAREST;
    auto toAddressMode = [](GLenum wrap) {
        if (wrap == GL_REPEAT) return VK_SAMPLER_ADDRESS_MODE_REPEAT;
        return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    };
    info.addressModeU = toAddressMode(tex.wrapS);
    info.addressModeV = toAddressMode(tex.wrapT);
    info.addressModeW = toAddressMode(tex.wrapS);
    info.minLod = 0.0f;
    info.maxLod = 1.0f;
    vkCreateSampler(dev, &info, nullptr, &tex.sampler);
    tex.samplerDirty = false;
}

WebGLTexture WebGLVkContext::createTexture() {
    GLuint id = nextTextureId_++;
    textures_[id] = VkTextureResource{};
    return {id};
}

void WebGLVkContext::deleteTexture(WebGLTexture tex) {
    auto it = textures_.find(tex.id);
    if (it != textures_.end()) {
        VkDevice dev = context_.device();
        if (it->second.sampler != VK_NULL_HANDLE) vkDestroySampler(dev, it->second.sampler, nullptr);
        if (it->second.view != VK_NULL_HANDLE) vkDestroyImageView(dev, it->second.view, nullptr);
        if (it->second.image != VK_NULL_HANDLE) vkDestroyImage(dev, it->second.image, nullptr);
        if (it->second.memory != VK_NULL_HANDLE) vkFreeMemory(dev, it->second.memory, nullptr);
        textures_.erase(it);
    }
}

void WebGLVkContext::bindTexture(GLenum /*target*/, WebGLTexture tex) {
    if (activeTextureUnit_ < boundTextures2D_.size()) {
        boundTextures2D_[activeTextureUnit_] = tex.id;
    }
}

void WebGLVkContext::activeTexture(GLenum texture) {
    if (texture >= GL_TEXTURE0 && texture < GL_TEXTURE0 + 32) {
        activeTextureUnit_ = texture - GL_TEXTURE0;
    }
}

void WebGLVkContext::texParameteri(GLenum /*target*/, GLenum pname, GLint param) {
    GLuint texId = (activeTextureUnit_ < boundTextures2D_.size()) ? boundTextures2D_[activeTextureUnit_] : 0;
    if (texId == 0) return;
    VkTextureResource& tex = textures_[texId];

    if (pname == GL_TEXTURE_MIN_FILTER) { tex.minFilter = param; tex.samplerDirty = true; }
    else if (pname == GL_TEXTURE_MAG_FILTER) { tex.magFilter = param; tex.samplerDirty = true; }
    else if (pname == GL_TEXTURE_WRAP_S) { tex.wrapS = param; tex.samplerDirty = true; }
    else if (pname == GL_TEXTURE_WRAP_T) { tex.wrapT = param; tex.samplerDirty = true; }
}

void WebGLVkContext::texImage2D(GLenum /*target*/, GLint /*level*/, GLint internalformat,
                                GLsizei width, GLsizei height, GLint /*border*/,
                                GLenum format, GLenum type, const void* pixels) {
    GLuint texId = (activeTextureUnit_ < boundTextures2D_.size()) ? boundTextures2D_[activeTextureUnit_] : 0;
    if (texId == 0 || width <= 0 || height <= 0) return;

    submitAndFlush();
    VkTextureResource& tex = textures_[texId];
    VkDevice dev = context_.device();

    if (tex.view != VK_NULL_HANDLE) { vkDestroyImageView(dev, tex.view, nullptr); tex.view = VK_NULL_HANDLE; }
    if (tex.image != VK_NULL_HANDLE) { vkDestroyImage(dev, tex.image, nullptr); tex.image = VK_NULL_HANDLE; }
    if (tex.memory != VK_NULL_HANDLE) { vkFreeMemory(dev, tex.memory, nullptr); tex.memory = VK_NULL_HANDLE; }

    tex.width = width;
    tex.height = height;

    VkFormat vkFormat = VK_FORMAT_R8G8B8A8_UNORM;
    uint32_t bpp = 4;

    if (internalformat == GL_R32F || (format == GL_RED && type == GL_FLOAT)) {
        vkFormat = VK_FORMAT_R32_SFLOAT;
        bpp = 4;
    } else if (internalformat == GL_RG32F || (format == GL_RG && type == GL_FLOAT)) {
        vkFormat = VK_FORMAT_R32G32_SFLOAT;
        bpp = 8;
    } else if (internalformat == GL_RGBA32F || (format == GL_RGBA && type == GL_FLOAT)) {
        vkFormat = VK_FORMAT_R32G32B32A32_SFLOAT;
        bpp = 16;
    } else if (internalformat == GL_R8 || format == GL_RED) {
        vkFormat = VK_FORMAT_R8_UNORM;
        bpp = 1;
    } else if (internalformat == GL_RG8 || format == GL_RG) {
        vkFormat = VK_FORMAT_R8G8_UNORM;
        bpp = 2;
    } else {
        vkFormat = VK_FORMAT_R8G8B8A8_UNORM;
        bpp = 4;
    }

    tex.format = vkFormat;
    tex.bytesPerPixel = bpp;

    VkImageUsageFlags usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                              VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    context_.createImage(width, height, tex.format, VK_IMAGE_TILING_OPTIMAL, usage,
                         VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, tex.image, tex.memory);

    VkImageViewCreateInfo viewInfo{};
    viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewInfo.image = tex.image;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = tex.format;
    viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    viewInfo.subresourceRange.baseMipLevel = 0;
    viewInfo.subresourceRange.levelCount = 1;
    viewInfo.subresourceRange.baseArrayLayer = 0;
    viewInfo.subresourceRange.layerCount = 1;
    vkCreateImageView(dev, &viewInfo, nullptr, &tex.view);

    if (pixels) {
        VkDeviceSize imgSize = static_cast<VkDeviceSize>(width) * height * bpp;
        VkBuffer stagingBuf = VK_NULL_HANDLE;
        VkDeviceMemory stagingMem = VK_NULL_HANDLE;
        context_.createBuffer(imgSize, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                              VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                              stagingBuf, stagingMem);

        void* mapped = nullptr;
        vkMapMemory(dev, stagingMem, 0, imgSize, 0, &mapped);
        std::memcpy(mapped, pixels, imgSize);
        vkUnmapMemory(dev, stagingMem);

        VkCommandBuffer cmd = context_.beginSingleTimeCommands();
        context_.transitionImageLayout(tex.image, tex.format, VK_IMAGE_LAYOUT_UNDEFINED,
                                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, cmd);
        context_.copyBufferToImage(stagingBuf, tex.image, width, height, cmd);
        context_.transitionImageLayout(tex.image, tex.format, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                       VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, cmd);
        context_.endSingleTimeCommands(cmd);
        tex.currentLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

        vkDestroyBuffer(dev, stagingBuf, nullptr);
        vkFreeMemory(dev, stagingMem, nullptr);
    } else {
        VkCommandBuffer cmd = context_.beginSingleTimeCommands();
        context_.transitionImageLayout(tex.image, tex.format, VK_IMAGE_LAYOUT_UNDEFINED,
                                       VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, cmd);
        context_.endSingleTimeCommands(cmd);
        tex.currentLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    }
}

void WebGLVkContext::texSubImage2D(GLenum /*target*/, GLint /*level*/, GLint xoffset, GLint yoffset,
                                   GLsizei width, GLsizei height,
                                   GLenum format, GLenum type, const void* pixels) {
    GLuint texId = (activeTextureUnit_ < boundTextures2D_.size()) ? boundTextures2D_[activeTextureUnit_] : 0;
    if (texId == 0 || !pixels || width <= 0 || height <= 0) return;

    auto it = textures_.find(texId);
    if (it == textures_.end() || !it->second.isValid()) return;
    VkTextureResource& tex = it->second;

    submitAndFlush();

    uint32_t bpp = tex.bytesPerPixel;
    if (type == GL_FLOAT) {
        if (format == GL_RED) bpp = 4;
        else if (format == GL_RG) bpp = 8;
        else if (format == GL_RGBA) bpp = 16;
    } else if (type == GL_UNSIGNED_BYTE) {
        if (format == GL_RED) bpp = 1;
        else if (format == GL_RG) bpp = 2;
        else if (format == GL_RGBA) bpp = 4;
    }

    VkDeviceSize uploadSize = static_cast<VkDeviceSize>(width) * height * bpp;
    VkBuffer stagingBuf = VK_NULL_HANDLE;
    VkDeviceMemory stagingMem = VK_NULL_HANDLE;
    VkDevice dev = context_.device();

    context_.createBuffer(uploadSize, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                          VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                          stagingBuf, stagingMem);

    void* mapped = nullptr;
    vkMapMemory(dev, stagingMem, 0, uploadSize, 0, &mapped);
    std::memcpy(mapped, pixels, uploadSize);
    vkUnmapMemory(dev, stagingMem);

    VkCommandBuffer cmd = context_.beginSingleTimeCommands();
    context_.transitionImageLayout(tex.image, tex.format, tex.currentLayout,
                                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, cmd);

    VkBufferImageCopy region{};
    region.bufferOffset = 0;
    region.bufferRowLength = 0;
    region.bufferImageHeight = 0;
    region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    region.imageSubresource.mipLevel = 0;
    region.imageSubresource.baseArrayLayer = 0;
    region.imageSubresource.layerCount = 1;
    region.imageOffset = {xoffset, yoffset, 0};
    region.imageExtent = {static_cast<uint32_t>(width), static_cast<uint32_t>(height), 1};

    vkCmdCopyBufferToImage(cmd, stagingBuf, tex.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

    context_.transitionImageLayout(tex.image, tex.format, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                   VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, cmd);
    context_.endSingleTimeCommands(cmd);
    tex.currentLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    vkDestroyBuffer(dev, stagingBuf, nullptr);
    vkFreeMemory(dev, stagingMem, nullptr);
}

void WebGLVkContext::generateMipmap(GLenum /*target*/) {}

WebGLFramebuffer WebGLVkContext::createFramebuffer() {
    GLuint id = nextFboId_++;
    framebuffers_[id] = VkFramebufferResource{};
    return {id};
}

void WebGLVkContext::deleteFramebuffer(WebGLFramebuffer fb) {
    framebuffers_.erase(fb.id);
    if (currentFboId_ == fb.id) currentFboId_ = 0;
}

void WebGLVkContext::bindFramebuffer(GLenum /*target*/, WebGLFramebuffer fb) {
    if (currentFboId_ != fb.id) {
        submitAndFlush();
        currentFboId_ = fb.id;
    }
}

void WebGLVkContext::framebufferTexture2D(GLenum /*target*/, GLenum attachment, GLenum /*textarget*/,
                                         WebGLTexture tex, GLint /*level*/) {
    if (currentFboId_ == 0) return;
    VkFramebufferResource& fbo = framebuffers_[currentFboId_];
    if (attachment == GL_COLOR_ATTACHMENT0) {
        fbo.colorAttachmentTex = tex.id;
    } else if (attachment == GL_DEPTH_ATTACHMENT) {
        fbo.depthAttachmentTex = tex.id;
    }
}

GLenum WebGLVkContext::checkFramebufferStatus(GLenum /*target*/) {
    return GL_FRAMEBUFFER_COMPLETE;
}

void WebGLVkContext::readPixels(GLint x, GLint y, GLsizei width, GLsizei height,
                                GLenum /*format*/, GLenum /*type*/, void* pixels) {
    if (!pixels || width <= 0 || height <= 0) return;

    submitAndFlush();

    if (currentFboId_ == 0) {
        std::vector<uint8_t> canvasData;
        if (!canvas_.readCanvasPixels(canvasData)) return;

        size_t canvasW = canvas_.width();
        size_t canvasH = canvas_.height();

        uint8_t* dst = static_cast<uint8_t*>(pixels);
        for (GLsizei row = 0; row < height; ++row) {
            GLint srcY = y + row;
            if (srcY >= 0 && static_cast<size_t>(srcY) < canvasH) {
                GLint srcX = std::max(0, x);
                GLsizei copyW = std::min(width, static_cast<GLsizei>(canvasW - srcX));
                if (copyW > 0) {
                    const uint8_t* srcRow = canvasData.data() + (srcY * canvasW + srcX) * 4;
                    std::memcpy(dst + row * width * 4, srcRow, copyW * 4);
                }
            }
        }
    } else {
        auto itFbo = framebuffers_.find(currentFboId_);
        if (itFbo == framebuffers_.end() || itFbo->second.colorAttachmentTex == 0) return;
        auto itTex = textures_.find(itFbo->second.colorAttachmentTex);
        if (itTex == textures_.end() || !itTex->second.isValid()) return;

        VkTextureResource& tex = itTex->second;
        VkDevice dev = context_.device();
        VkDeviceSize imgSize = static_cast<VkDeviceSize>(tex.width) * tex.height * tex.bytesPerPixel;

        VkBuffer readbackBuf = VK_NULL_HANDLE;
        VkDeviceMemory readbackMem = VK_NULL_HANDLE;
        context_.createBuffer(imgSize, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                              VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                              readbackBuf, readbackMem);

        VkCommandBuffer cmd = context_.beginSingleTimeCommands();
        context_.transitionImageLayout(tex.image, tex.format, tex.currentLayout,
                                       VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, cmd);

        VkBufferImageCopy copyRegion{};
        copyRegion.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        copyRegion.imageSubresource.layerCount = 1;
        copyRegion.imageExtent = {tex.width, tex.height, 1};
        vkCmdCopyImageToBuffer(cmd, tex.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                               readbackBuf, 1, &copyRegion);

        context_.transitionImageLayout(tex.image, tex.format, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                       tex.currentLayout, cmd);
        context_.endSingleTimeCommands(cmd);

        void* mapped = nullptr;
        if (vkMapMemory(dev, readbackMem, 0, imgSize, 0, &mapped) == VK_SUCCESS) {
            const uint8_t* srcData = static_cast<const uint8_t*>(mapped);
            uint8_t* dst = static_cast<uint8_t*>(pixels);
            for (GLsizei row = 0; row < height; ++row) {
                GLint srcY = y + row;
                if (srcY >= 0 && static_cast<size_t>(srcY) < tex.height) {
                    GLint srcX = std::max(0, x);
                    GLsizei copyW = std::min(width, static_cast<GLsizei>(tex.width - srcX));
                    if (copyW > 0) {
                        const uint8_t* srcRow = srcData + (srcY * tex.width + srcX) * tex.bytesPerPixel;
                        std::memcpy(dst + row * width * tex.bytesPerPixel, srcRow, copyW * tex.bytesPerPixel);
                    }
                }
            }
            vkUnmapMemory(dev, readbackMem);
        }

        vkDestroyBuffer(dev, readbackBuf, nullptr);
        vkFreeMemory(dev, readbackMem, nullptr);
    }
}

} // namespace bro::webgl::vk
