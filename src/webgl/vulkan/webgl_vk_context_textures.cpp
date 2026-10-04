#include "webgl/vulkan/webgl_vk_context.h"
#include "util/log.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace bro::webgl::vk {

static void copyAndProcessPixels(uint8_t* dst, const void* src, GLsizei width, GLsizei height,
                                 uint32_t bpp, GLint unpackAlignment, bool flipY, bool premultiplyAlpha) {
    if (!src || !dst) return;
    size_t srcRowBytes = width * bpp;
    if (unpackAlignment > 1) {
        srcRowBytes = (srcRowBytes + unpackAlignment - 1) / unpackAlignment * unpackAlignment;
    }
    size_t dstRowBytes = width * bpp;
    for (GLsizei r = 0; r < height; ++r) {
        size_t dstRow = flipY ? (height - 1 - r) : r;
        const uint8_t* srcRowPtr = static_cast<const uint8_t*>(src) + r * srcRowBytes;
        uint8_t* dstRowPtr = dst + dstRow * dstRowBytes;
        if (premultiplyAlpha && bpp == 4) {
            for (GLsizei x = 0; x < width; ++x) {
                uint8_t r_col = srcRowPtr[x * 4 + 0];
                uint8_t g_col = srcRowPtr[x * 4 + 1];
                uint8_t b_col = srcRowPtr[x * 4 + 2];
                uint8_t a_col = srcRowPtr[x * 4 + 3];
                dstRowPtr[x * 4 + 0] = static_cast<uint8_t>((r_col * a_col + 127) / 255);
                dstRowPtr[x * 4 + 1] = static_cast<uint8_t>((g_col * a_col + 127) / 255);
                dstRowPtr[x * 4 + 2] = static_cast<uint8_t>((b_col * a_col + 127) / 255);
                dstRowPtr[x * 4 + 3] = a_col;
            }
        } else {
            std::memcpy(dstRowPtr, srcRowPtr, dstRowBytes);
        }
    }
}

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
    info.maxLod = (tex.mipLevels > 1) ? static_cast<float>(tex.mipLevels) : 1.0f;
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

void WebGLVkContext::bindTexture(GLenum target, WebGLTexture tex) {
    if (activeTextureUnit_ >= boundTextures2D_.size()) return;
    if (target == 0x8513 /* GL_TEXTURE_CUBE_MAP */) {
        boundTexturesCubeMap_[activeTextureUnit_] = tex.id;
    } else if (target == 0x8C1A /* GL_TEXTURE_2D_ARRAY */) {
        boundTextures2DArray_[activeTextureUnit_] = tex.id;
    } else if (target == 0x806F /* GL_TEXTURE_3D */) {
        boundTextures3D_[activeTextureUnit_] = tex.id;
    } else {
        boundTextures2D_[activeTextureUnit_] = tex.id;
    }
}

void WebGLVkContext::activeTexture(GLenum texture) {
    if (texture >= GL_TEXTURE0 && texture < GL_TEXTURE0 + 32) {
        activeTextureUnit_ = texture - GL_TEXTURE0;
    }
}

void WebGLVkContext::texParameteri(GLenum target, GLenum pname, GLint param) {
    GLuint texId = 0;
    if (activeTextureUnit_ < boundTextures2D_.size()) {
        if (target == 0x8513) texId = boundTexturesCubeMap_[activeTextureUnit_];
        else if (target == 0x8C1A || target == 0x806F) texId = boundTextures2DArray_[activeTextureUnit_];
        else texId = boundTextures2D_[activeTextureUnit_];
    }
    if (texId == 0) return;
    VkTextureResource& tex = textures_[texId];

    if (pname == GL_TEXTURE_MIN_FILTER) { tex.minFilter = param; tex.samplerDirty = true; }
    else if (pname == GL_TEXTURE_MAG_FILTER) { tex.magFilter = param; tex.samplerDirty = true; }
    else if (pname == GL_TEXTURE_WRAP_S) { tex.wrapS = param; tex.samplerDirty = true; }
    else if (pname == GL_TEXTURE_WRAP_T) { tex.wrapT = param; tex.samplerDirty = true; }
}

void WebGLVkContext::texStorage2D(GLenum target, GLsizei /*levels*/, GLenum internalformat,
                                  GLsizei width, GLsizei height) {
    texImage2D(target, 0, static_cast<GLint>(internalformat), width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
}

void WebGLVkContext::texImage2D(GLenum target, GLint level, GLint internalformat,
                                GLsizei width, GLsizei height, GLint /*border*/,
                                GLenum format, GLenum type, const void* pixels) {
    bool isCubeFace = (target >= 0x8515 && target <= 0x851A);
    uint32_t faceIndex = isCubeFace ? (target - 0x8515) : 0;
    GLuint texId = 0;
    if (activeTextureUnit_ < boundTextures2D_.size()) {
        if (isCubeFace || target == 0x8513) texId = boundTexturesCubeMap_[activeTextureUnit_];
        else texId = boundTextures2D_[activeTextureUnit_];
    }
    if (texId == 0 || width <= 0 || height <= 0) return;

    submitAndFlush();
    VkTextureResource& tex = textures_[texId];
    VkDevice dev = context_.device();

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
    } else if (internalformat == 0x81A5 /* DEPTH_COMPONENT16 */ || internalformat == 0x81A6 /* DEPTH_COMPONENT24 */ || format == 0x1902 /* DEPTH_COMPONENT */) {
        vkFormat = VK_FORMAT_D32_SFLOAT;
        bpp = 4;
    } else if (internalformat == 0x88F0 /* DEPTH24_STENCIL8 */ || format == 0x84F9 /* DEPTH_STENCIL */) {
        vkFormat = VK_FORMAT_D32_SFLOAT_S8_UINT;
        bpp = 4;
    } else {
        vkFormat = VK_FORMAT_R8G8B8A8_UNORM;
        bpp = 4;
    }

    VkImageUsageFlags usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                              VK_IMAGE_USAGE_SAMPLED_BIT;
    if (vkFormat == VK_FORMAT_D32_SFLOAT || vkFormat == VK_FORMAT_D32_SFLOAT_S8_UINT) {
        usage |= VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
    } else {
        usage |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    }

    if (isCubeFace) {
        tex.target = 0x8513;
        if (tex.image == VK_NULL_HANDLE || tex.width != static_cast<uint32_t>(width) || tex.height != static_cast<uint32_t>(height)) {
            if (tex.view != VK_NULL_HANDLE) { vkDestroyImageView(dev, tex.view, nullptr); tex.view = VK_NULL_HANDLE; }
            if (tex.image != VK_NULL_HANDLE) { vkDestroyImage(dev, tex.image, nullptr); tex.image = VK_NULL_HANDLE; }
            if (tex.memory != VK_NULL_HANDLE) { vkFreeMemory(dev, tex.memory, nullptr); tex.memory = VK_NULL_HANDLE; }

            tex.width = width;
            tex.height = height;
            tex.format = vkFormat;
            tex.bytesPerPixel = bpp;
            tex.mipLevels = 1;

            context_.createImage(width, height, tex.format, VK_IMAGE_TILING_OPTIMAL, usage,
                                 VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, tex.image, tex.memory,
                                 1, 6, VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT);

            VkImageViewCreateInfo viewInfo{};
            viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
            viewInfo.image = tex.image;
            viewInfo.viewType = VK_IMAGE_VIEW_TYPE_CUBE;
            viewInfo.format = tex.format;
            viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            viewInfo.subresourceRange.baseMipLevel = 0;
            viewInfo.subresourceRange.levelCount = 1;
            viewInfo.subresourceRange.baseArrayLayer = 0;
            viewInfo.subresourceRange.layerCount = 6;
            vkCreateImageView(dev, &viewInfo, nullptr, &tex.view);

            VkCommandBuffer cmd = context_.beginSingleTimeCommands();
            context_.transitionImageLayout(tex.image, tex.format, VK_IMAGE_LAYOUT_UNDEFINED,
                                           VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, cmd, 1, 0, 6, 0);
            context_.endSingleTimeCommands(cmd);
            tex.currentLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        }

        if (pixels) {
            VkDeviceSize imgSize = static_cast<VkDeviceSize>(width) * height * bpp;
            VkBuffer stagingBuf = VK_NULL_HANDLE;
            VkDeviceMemory stagingMem = VK_NULL_HANDLE;
            context_.createBuffer(imgSize, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                                  VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                                  stagingBuf, stagingMem);

            void* mapped = nullptr;
            vkMapMemory(dev, stagingMem, 0, imgSize, 0, &mapped);
            copyAndProcessPixels(static_cast<uint8_t*>(mapped), pixels, width, height, bpp,
                                 unpackAlignment_, unpackFlipY_, unpackPremultiplyAlpha_);
            vkUnmapMemory(dev, stagingMem);

            VkCommandBuffer cmd = context_.beginSingleTimeCommands();
            context_.transitionImageLayout(tex.image, tex.format, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, cmd, 1, 0, 1, faceIndex);
            context_.copyBufferToImage(stagingBuf, tex.image, width, height, cmd, 0, faceIndex, 1);
            context_.transitionImageLayout(tex.image, tex.format, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                           VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, cmd, 1, 0, 1, faceIndex);
            context_.endSingleTimeCommands(cmd);

            vkDestroyBuffer(dev, stagingBuf, nullptr);
            vkFreeMemory(dev, stagingMem, nullptr);
        }
        return;
    }

    // Standard 2D texture
    tex.target = GL_TEXTURE_2D;
    uint32_t numLevels = std::max(1u, static_cast<uint32_t>(std::floor(std::log2(std::max(width, height)))) + 1);

    if (level == 0 || tex.image == VK_NULL_HANDLE) {
        if (tex.view != VK_NULL_HANDLE) { vkDestroyImageView(dev, tex.view, nullptr); tex.view = VK_NULL_HANDLE; }
        if (tex.image != VK_NULL_HANDLE) { vkDestroyImage(dev, tex.image, nullptr); tex.image = VK_NULL_HANDLE; }
        if (tex.memory != VK_NULL_HANDLE) { vkFreeMemory(dev, tex.memory, nullptr); tex.memory = VK_NULL_HANDLE; }

        tex.width = width;
        tex.height = height;
        tex.format = vkFormat;
        tex.bytesPerPixel = bpp;
        tex.mipLevels = numLevels;

        context_.createImage(width, height, tex.format, VK_IMAGE_TILING_OPTIMAL, usage,
                             VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, tex.image, tex.memory,
                             numLevels, 1, 0);

        VkImageViewCreateInfo viewInfo{};
        viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        viewInfo.image = tex.image;
        viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        viewInfo.format = tex.format;
        bool isDepth = (vkFormat == VK_FORMAT_D32_SFLOAT || vkFormat == VK_FORMAT_D32_SFLOAT_S8_UINT);
        viewInfo.subresourceRange.aspectMask = isDepth ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
        viewInfo.subresourceRange.baseMipLevel = 0;
        viewInfo.subresourceRange.levelCount = numLevels;
        viewInfo.subresourceRange.baseArrayLayer = 0;
        viewInfo.subresourceRange.layerCount = 1;
        vkCreateImageView(dev, &viewInfo, nullptr, &tex.view);

        VkCommandBuffer cmd = context_.beginSingleTimeCommands();
        VkImageLayout initialLayout = isDepth ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        context_.transitionImageLayout(tex.image, tex.format, VK_IMAGE_LAYOUT_UNDEFINED,
                                       initialLayout, cmd, numLevels, 0, 1, 0);
        context_.endSingleTimeCommands(cmd);
        tex.currentLayout = initialLayout;
    }

    if (pixels) {
        VkDeviceSize imgSize = static_cast<VkDeviceSize>(width) * height * bpp;
        VkBuffer stagingBuf = VK_NULL_HANDLE;
        VkDeviceMemory stagingMem = VK_NULL_HANDLE;
        context_.createBuffer(imgSize, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                              VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                              stagingBuf, stagingMem);

        void* mapped = nullptr;
        vkMapMemory(dev, stagingMem, 0, imgSize, 0, &mapped);
        copyAndProcessPixels(static_cast<uint8_t*>(mapped), pixels, width, height, bpp,
                             unpackAlignment_, unpackFlipY_, unpackPremultiplyAlpha_);
        vkUnmapMemory(dev, stagingMem);

        VkCommandBuffer cmd = context_.beginSingleTimeCommands();
        context_.transitionImageLayout(tex.image, tex.format, tex.currentLayout,
                                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, cmd, 1, level, 1, 0);
        context_.copyBufferToImage(stagingBuf, tex.image, width, height, cmd, level, 0, 1);
        context_.transitionImageLayout(tex.image, tex.format, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                       VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, cmd, 1, level, 1, 0);
        context_.endSingleTimeCommands(cmd);
        tex.currentLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

        vkDestroyBuffer(dev, stagingBuf, nullptr);
        vkFreeMemory(dev, stagingMem, nullptr);
    }
}

void WebGLVkContext::texSubImage2D(GLenum /*target*/, GLint level, GLint xoffset, GLint yoffset,
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
    copyAndProcessPixels(static_cast<uint8_t*>(mapped), pixels, width, height, bpp,
                         unpackAlignment_, unpackFlipY_, unpackPremultiplyAlpha_);
    vkUnmapMemory(dev, stagingMem);

    VkCommandBuffer cmd = context_.beginSingleTimeCommands();
    context_.transitionImageLayout(tex.image, tex.format, tex.currentLayout,
                                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, cmd, 1, level, 1, 0);

    VkBufferImageCopy region{};
    region.bufferOffset = 0;
    region.bufferRowLength = 0;
    region.bufferImageHeight = 0;
    region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    region.imageSubresource.mipLevel = level;
    region.imageSubresource.baseArrayLayer = 0;
    region.imageSubresource.layerCount = 1;
    region.imageOffset = {xoffset, yoffset, 0};
    region.imageExtent = {static_cast<uint32_t>(width), static_cast<uint32_t>(height), 1};

    vkCmdCopyBufferToImage(cmd, stagingBuf, tex.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

    context_.transitionImageLayout(tex.image, tex.format, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                   VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, cmd, 1, level, 1, 0);
    context_.endSingleTimeCommands(cmd);
    tex.currentLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    vkDestroyBuffer(dev, stagingBuf, nullptr);
    vkFreeMemory(dev, stagingMem, nullptr);
}

void WebGLVkContext::generateMipmap(GLenum target) {
    GLuint texId = 0;
    if (activeTextureUnit_ < boundTextures2D_.size()) {
        if (target == 0x8513) texId = boundTexturesCubeMap_[activeTextureUnit_];
        else if (target == 0x8C1A) texId = boundTextures2DArray_[activeTextureUnit_];
        else texId = boundTextures2D_[activeTextureUnit_];
    }
    if (texId == 0) return;
    auto it = textures_.find(texId);
    if (it == textures_.end() || !it->second.isValid() || it->second.mipLevels <= 1) return;
    VkTextureResource& tex = it->second;
    submitAndFlush();

    VkCommandBuffer cmd = context_.beginSingleTimeCommands();
    VkImageMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.image = tex.image;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    barrier.subresourceRange.baseArrayLayer = 0;
    barrier.subresourceRange.layerCount = 1;
    barrier.subresourceRange.levelCount = 1;

    int32_t mipWidth = tex.width;
    int32_t mipHeight = tex.height;

    for (uint32_t i = 1; i < tex.mipLevels; ++i) {
        barrier.subresourceRange.baseMipLevel = i - 1;
        barrier.oldLayout = (i == 1) ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        barrier.srcAccessMask = (i == 1) ? VK_ACCESS_SHADER_READ_BIT : VK_ACCESS_TRANSFER_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
                             0, nullptr, 0, nullptr, 1, &barrier);

        VkImageBlit blit{};
        blit.srcOffsets[0] = {0, 0, 0};
        blit.srcOffsets[1] = {mipWidth, mipHeight, 1};
        blit.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        blit.srcSubresource.mipLevel = i - 1;
        blit.srcSubresource.baseArrayLayer = 0;
        blit.srcSubresource.layerCount = 1;
        blit.dstOffsets[0] = {0, 0, 0};
        blit.dstOffsets[1] = {std::max(1, mipWidth / 2), std::max(1, mipHeight / 2), 1};
        blit.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        blit.dstSubresource.mipLevel = i;
        blit.dstSubresource.baseArrayLayer = 0;
        blit.dstSubresource.layerCount = 1;

        barrier.subresourceRange.baseMipLevel = i;
        barrier.oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        barrier.srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
        barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
                             0, nullptr, 0, nullptr, 1, &barrier);

        vkCmdBlitImage(cmd, tex.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                       tex.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                       1, &blit, VK_FILTER_LINEAR);

        barrier.subresourceRange.baseMipLevel = i - 1;
        barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        barrier.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0,
                             0, nullptr, 0, nullptr, 1, &barrier);

        barrier.subresourceRange.baseMipLevel = i;
        barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0,
                             0, nullptr, 0, nullptr, 1, &barrier);

        if (mipWidth > 1) mipWidth /= 2;
        if (mipHeight > 1) mipHeight /= 2;
    }
    context_.endSingleTimeCommands(cmd);
    tex.currentLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
}

void WebGLVkContext::texImage3D(GLenum target, GLint level, GLint /*internalformat*/,
                                GLsizei width, GLsizei height, GLsizei depth, GLint /*border*/,
                                GLenum /*format*/, GLenum /*type*/, const void* pixels) {
    GLuint texId = (activeTextureUnit_ < boundTextures2DArray_.size()) ? boundTextures2DArray_[activeTextureUnit_] : 0;
    if (texId == 0 || width <= 0 || height <= 0 || depth <= 0) return;
    submitAndFlush();
    VkTextureResource& tex = textures_[texId];
    VkDevice dev = context_.device();

    if (tex.view != VK_NULL_HANDLE) { vkDestroyImageView(dev, tex.view, nullptr); tex.view = VK_NULL_HANDLE; }
    if (tex.image != VK_NULL_HANDLE) { vkDestroyImage(dev, tex.image, nullptr); tex.image = VK_NULL_HANDLE; }
    if (tex.memory != VK_NULL_HANDLE) { vkFreeMemory(dev, tex.memory, nullptr); tex.memory = VK_NULL_HANDLE; }

    tex.width = width;
    tex.height = height;
    tex.depth = depth;
    tex.target = target;
    tex.format = VK_FORMAT_R8G8B8A8_UNORM;
    tex.bytesPerPixel = 4;
    tex.mipLevels = 1;

    VkImageUsageFlags usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                              VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    context_.createImage(width, height, tex.format, VK_IMAGE_TILING_OPTIMAL, usage,
                         VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, tex.image, tex.memory,
                         1, depth, 0);

    VkImageViewCreateInfo viewInfo{};
    viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewInfo.image = tex.image;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
    viewInfo.format = tex.format;
    viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    viewInfo.subresourceRange.baseMipLevel = 0;
    viewInfo.subresourceRange.levelCount = 1;
    viewInfo.subresourceRange.baseArrayLayer = 0;
    viewInfo.subresourceRange.layerCount = depth;
    vkCreateImageView(dev, &viewInfo, nullptr, &tex.view);

    if (pixels) {
        VkDeviceSize imgSize = static_cast<VkDeviceSize>(width) * height * depth * tex.bytesPerPixel;
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
                                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, cmd, 1, 0, depth, 0);
        context_.copyBufferToImage(stagingBuf, tex.image, width, height, cmd, 0, 0, depth);
        context_.transitionImageLayout(tex.image, tex.format, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                       VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, cmd, 1, 0, depth, 0);
        context_.endSingleTimeCommands(cmd);
        tex.currentLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

        vkDestroyBuffer(dev, stagingBuf, nullptr);
        vkFreeMemory(dev, stagingMem, nullptr);
    } else {
        VkCommandBuffer cmd = context_.beginSingleTimeCommands();
        context_.transitionImageLayout(tex.image, tex.format, VK_IMAGE_LAYOUT_UNDEFINED,
                                       VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, cmd, 1, 0, depth, 0);
        context_.endSingleTimeCommands(cmd);
        tex.currentLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    }
}

void WebGLVkContext::texStorage3D(GLenum target, GLsizei /*levels*/, GLenum internalformat,
                                  GLsizei width, GLsizei height, GLsizei depth) {
    texImage3D(target, 0, static_cast<GLint>(internalformat), width, height, depth, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
}

void WebGLVkContext::texSubImage3D(GLenum /*target*/, GLint level,
                                   GLint xoffset, GLint yoffset, GLint zoffset,
                                   GLsizei width, GLsizei height, GLsizei depth,
                                   GLenum /*format*/, GLenum /*type*/, const void* pixels) {
    GLuint texId = (activeTextureUnit_ < boundTextures2DArray_.size()) ? boundTextures2DArray_[activeTextureUnit_] : 0;
    if (texId == 0 || !pixels || width <= 0 || height <= 0 || depth <= 0) return;
    auto it = textures_.find(texId);
    if (it == textures_.end() || !it->second.isValid()) return;
    VkTextureResource& tex = it->second;

    submitAndFlush();
    VkDevice dev = context_.device();
    VkDeviceSize uploadSize = static_cast<VkDeviceSize>(width) * height * depth * tex.bytesPerPixel;
    VkBuffer stagingBuf = VK_NULL_HANDLE;
    VkDeviceMemory stagingMem = VK_NULL_HANDLE;
    context_.createBuffer(uploadSize, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                          VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                          stagingBuf, stagingMem);

    void* mapped = nullptr;
    vkMapMemory(dev, stagingMem, 0, uploadSize, 0, &mapped);
    std::memcpy(mapped, pixels, uploadSize);
    vkUnmapMemory(dev, stagingMem);

    VkCommandBuffer cmd = context_.beginSingleTimeCommands();
    context_.transitionImageLayout(tex.image, tex.format, tex.currentLayout,
                                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, cmd, 1, level, depth, zoffset);

    VkBufferImageCopy region{};
    region.bufferOffset = 0;
    region.bufferRowLength = 0;
    region.bufferImageHeight = 0;
    region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    region.imageSubresource.mipLevel = level;
    region.imageSubresource.baseArrayLayer = zoffset;
    region.imageSubresource.layerCount = depth;
    region.imageOffset = {xoffset, yoffset, 0};
    region.imageExtent = {static_cast<uint32_t>(width), static_cast<uint32_t>(height), 1};
    vkCmdCopyBufferToImage(cmd, stagingBuf, tex.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

    context_.transitionImageLayout(tex.image, tex.format, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                   VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, cmd, 1, level, depth, zoffset);
    context_.endSingleTimeCommands(cmd);
    tex.currentLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    vkDestroyBuffer(dev, stagingBuf, nullptr);
    vkFreeMemory(dev, stagingMem, nullptr);
}

void WebGLVkContext::updateSamplerObject(VkSamplerResource& smp) {
    if (smp.sampler != VK_NULL_HANDLE && !smp.samplerDirty) return;
    VkDevice dev = context_.device();
    if (smp.sampler != VK_NULL_HANDLE) {
        vkDestroySampler(dev, smp.sampler, nullptr);
        smp.sampler = VK_NULL_HANDLE;
    }
    VkSamplerCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    info.magFilter = (smp.magFilter == GL_NEAREST) ? VK_FILTER_NEAREST : VK_FILTER_LINEAR;
    info.minFilter = (smp.minFilter == GL_NEAREST || smp.minFilter == GL_NEAREST_MIPMAP_NEAREST) ? VK_FILTER_NEAREST : VK_FILTER_LINEAR;
    info.mipmapMode = (smp.minFilter == GL_LINEAR_MIPMAP_LINEAR || smp.minFilter == GL_NEAREST_MIPMAP_LINEAR) ? VK_SAMPLER_MIPMAP_MODE_LINEAR : VK_SAMPLER_MIPMAP_MODE_NEAREST;
    auto toAddressMode = [](GLenum wrap) {
        if (wrap == GL_REPEAT) return VK_SAMPLER_ADDRESS_MODE_REPEAT;
        if (wrap == GL_MIRRORED_REPEAT) return VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
        return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    };
    info.addressModeU = toAddressMode(smp.wrapS);
    info.addressModeV = toAddressMode(smp.wrapT);
    info.addressModeW = toAddressMode(smp.wrapR);
    info.minLod = std::max(0.0f, smp.minLod);
    info.maxLod = std::max(0.0f, smp.maxLod);
    if (smp.compareMode == GL_COMPARE_REF_TO_TEXTURE) {
        info.compareEnable = VK_TRUE;
        info.compareOp = glCompareOpToVk(smp.compareFunc);
    }
    vkCreateSampler(dev, &info, nullptr, &smp.sampler);
    smp.samplerDirty = false;
}

WebGLSampler WebGLVkContext::createSampler() {
    GLuint id = nextSamplerId_++;
    samplers_[id] = VkSamplerResource{};
    return {id};
}

void WebGLVkContext::deleteSampler(WebGLSampler s) {
    auto it = samplers_.find(s.id);
    if (it != samplers_.end()) {
        if (it->second.sampler != VK_NULL_HANDLE) {
            submitAndFlush();
            vkDestroySampler(context_.device(), it->second.sampler, nullptr);
        }
        samplers_.erase(it);
    }
    for (size_t i = 0; i < boundSamplers_.size(); ++i) {
        if (boundSamplers_[i] == s.id) {
            boundSamplers_[i] = 0;
        }
    }
}

void WebGLVkContext::bindSampler(GLuint unit, WebGLSampler s) {
    if (unit < boundSamplers_.size()) {
        boundSamplers_[unit] = s.id;
    }
}

void WebGLVkContext::samplerParameteri(WebGLSampler s, GLenum pname, GLint param) {
    auto it = samplers_.find(s.id);
    if (it == samplers_.end()) return;
    switch (pname) {
        case GL_TEXTURE_MIN_FILTER: it->second.minFilter = param; break;
        case GL_TEXTURE_MAG_FILTER: it->second.magFilter = param; break;
        case GL_TEXTURE_WRAP_S: it->second.wrapS = param; break;
        case GL_TEXTURE_WRAP_T: it->second.wrapT = param; break;
        case GL_TEXTURE_WRAP_R: it->second.wrapR = param; break;
        case GL_TEXTURE_COMPARE_MODE: it->second.compareMode = param; break;
        case GL_TEXTURE_COMPARE_FUNC: it->second.compareFunc = param; break;
        default: break;
    }
    it->second.samplerDirty = true;
}

void WebGLVkContext::samplerParameterf(WebGLSampler s, GLenum pname, GLfloat param) {
    auto it = samplers_.find(s.id);
    if (it == samplers_.end()) return;
    switch (pname) {
        case GL_TEXTURE_MIN_LOD: it->second.minLod = param; break;
        case GL_TEXTURE_MAX_LOD: it->second.maxLod = param; break;
        default: samplerParameteri(s, pname, static_cast<GLint>(param)); return;
    }
    it->second.samplerDirty = true;
}

GLint WebGLVkContext::getSamplerParameteri(WebGLSampler s, GLenum pname) {
    auto it = samplers_.find(s.id);
    if (it == samplers_.end()) return 0;
    switch (pname) {
        case GL_TEXTURE_MIN_FILTER: return it->second.minFilter;
        case GL_TEXTURE_MAG_FILTER: return it->second.magFilter;
        case GL_TEXTURE_WRAP_S: return it->second.wrapS;
        case GL_TEXTURE_WRAP_T: return it->second.wrapT;
        case GL_TEXTURE_WRAP_R: return it->second.wrapR;
        case GL_TEXTURE_COMPARE_MODE: return it->second.compareMode;
        case GL_TEXTURE_COMPARE_FUNC: return it->second.compareFunc;
        default: return 0;
    }
}

GLfloat WebGLVkContext::getSamplerParameterf(WebGLSampler s, GLenum pname) {
    auto it = samplers_.find(s.id);
    if (it == samplers_.end()) return 0.0f;
    switch (pname) {
        case GL_TEXTURE_MIN_LOD: return it->second.minLod;
        case GL_TEXTURE_MAX_LOD: return it->second.maxLod;
        default: return static_cast<GLfloat>(getSamplerParameteri(s, pname));
    }
}

GLboolean WebGLVkContext::isSampler(WebGLSampler s) {
    return (s.id != 0 && samplers_.find(s.id) != samplers_.end()) ? GL_TRUE : GL_FALSE;
}

void WebGLVkContext::copyTexImage2D(GLenum target, GLint level, GLenum internalformat,
                                    GLint x, GLint y, GLsizei width, GLsizei height, GLint border) {
    std::vector<uint8_t> pixels(static_cast<size_t>(width) * height * 4);
    readPixels(x, y, width, height, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
    texImage2D(target, level, internalformat, width, height, border, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
}

void WebGLVkContext::copyTexSubImage2D(GLenum target, GLint level,
                                       GLint xoffset, GLint yoffset,
                                       GLint x, GLint y, GLsizei width, GLsizei height) {
    std::vector<uint8_t> pixels(static_cast<size_t>(width) * height * 4);
    readPixels(x, y, width, height, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
    texSubImage2D(target, level, xoffset, yoffset, width, height, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
}

} // namespace bro::webgl::vk
