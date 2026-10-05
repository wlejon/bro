#include "webgl/vulkan/webgl_vk_context.h"
#include "webgl/vulkan/webgl_vk_formats.h"
#include "render/vulkan_util.h"
#include "util/log.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace bro::webgl::vk {

namespace {

constexpr GLenum kTextureCubeMap = 0x8513;
constexpr GLenum kCubeFacePosX = 0x8515;
constexpr GLenum kCubeFaceNegZ = 0x851A;

bool isCubeFace(GLenum target) { return target >= kCubeFacePosX && target <= kCubeFaceNegZ; }

// Rows of `src` (laid out per GL_UNPACK_ALIGNMENT) into tightly packed `dst`,
// flipped and/or alpha-premultiplied as the unpack state asks.
void copyAndProcessPixels(uint8_t* dst, const void* src, GLsizei width, GLsizei height,
                          uint32_t bpp, GLint unpackAlignment, bool flipY, bool premultiplyAlpha) {
    if (!src || !dst) return;
    size_t srcRowBytes = static_cast<size_t>(width) * bpp;
    if (unpackAlignment > 1) {
        srcRowBytes = (srcRowBytes + unpackAlignment - 1) / unpackAlignment * unpackAlignment;
    }
    const size_t dstRowBytes = static_cast<size_t>(width) * bpp;
    for (GLsizei r = 0; r < height; ++r) {
        const size_t dstRow = flipY ? (height - 1 - r) : r;
        const uint8_t* srcRowPtr = static_cast<const uint8_t*>(src) + r * srcRowBytes;
        uint8_t* dstRowPtr = dst + dstRow * dstRowBytes;
        if (premultiplyAlpha && bpp == 4) {
            for (GLsizei x = 0; x < width; ++x) {
                const uint8_t a = srcRowPtr[x * 4 + 3];
                dstRowPtr[x * 4 + 0] = static_cast<uint8_t>((srcRowPtr[x * 4 + 0] * a + 127) / 255);
                dstRowPtr[x * 4 + 1] = static_cast<uint8_t>((srcRowPtr[x * 4 + 1] * a + 127) / 255);
                dstRowPtr[x * 4 + 2] = static_cast<uint8_t>((srcRowPtr[x * 4 + 2] * a + 127) / 255);
                dstRowPtr[x * 4 + 3] = a;
            }
        } else {
            std::memcpy(dstRowPtr, srcRowPtr, dstRowBytes);
        }
    }
}

struct TexFormat {
    VkFormat format = VK_FORMAT_R8G8B8A8_UNORM;
    uint32_t bpp = 4;
};

TexFormat chooseFormat(GLint internalformat, GLenum format, GLenum type) {
    if (internalformat == GL_R32F || (format == GL_RED && type == GL_FLOAT)) return {VK_FORMAT_R32_SFLOAT, 4};
    if (internalformat == GL_RG32F || (format == GL_RG && type == GL_FLOAT)) return {VK_FORMAT_R32G32_SFLOAT, 8};
    if (internalformat == GL_RGBA32F || (format == GL_RGBA && type == GL_FLOAT)) return {VK_FORMAT_R32G32B32A32_SFLOAT, 16};
    if (internalformat == GL_R16F || (format == GL_RED && type == GL_HALF_FLOAT)) return {VK_FORMAT_R16_SFLOAT, 2};
    if (internalformat == GL_RG16F || (format == GL_RG && type == GL_HALF_FLOAT)) return {VK_FORMAT_R16G16_SFLOAT, 4};
    if (internalformat == GL_RGBA16F || (format == GL_RGBA && type == GL_HALF_FLOAT)) return {VK_FORMAT_R16G16B16A16_SFLOAT, 8};
    if (internalformat == GL_R8 || format == GL_RED) return {VK_FORMAT_R8_UNORM, 1};
    if (internalformat == GL_RG8 || format == GL_RG) return {VK_FORMAT_R8G8_UNORM, 2};
    if (internalformat == 0x81A5 /* DEPTH_COMPONENT16 */ || internalformat == 0x81A6 /* DEPTH_COMPONENT24 */ ||
        format == 0x1902 /* DEPTH_COMPONENT */)
        return {VK_FORMAT_D32_SFLOAT, 4};
    if (internalformat == 0x88F0 /* DEPTH24_STENCIL8 */ || format == 0x84F9 /* DEPTH_STENCIL */)
        return {VK_FORMAT_D32_SFLOAT_S8_UINT, 4};
    return {VK_FORMAT_R8G8B8A8_UNORM, 4};
}

bool isDepthFormat(VkFormat f) { return render::imageAspectFor(f) != VK_IMAGE_ASPECT_COLOR_BIT; }

// Bytes per pixel of client data in `format`/`type`, or `fallback`.
uint32_t clientBpp(GLenum format, GLenum type, uint32_t fallback) {
    const uint32_t comps = format == GL_RED ? 1 : format == GL_RG ? 2 : format == GL_RGBA ? 4 : 0;
    if (comps == 0) return fallback;
    if (type == GL_FLOAT) return comps * 4;
    if (type == GL_HALF_FLOAT) return comps * 2;
    if (type == GL_UNSIGNED_BYTE) return comps;
    return fallback;
}

} // namespace

WebGLTexture WebGLVkContext::createTexture() {
    GLuint id = nextTextureId_++;
    textures_[id] = VkTextureResource{};
    return {id};
}

void WebGLVkContext::deleteTexture(WebGLTexture tex) {
    auto it = tex.id != 0 ? textures_.find(tex.id) : textures_.end();
    if (it == textures_.end()) return;
    // Detached from the bound framebuffers (GL leaves other framebuffers
    // naming it, now incomplete); the open pass may render into it.
    for (GLuint fboId : {drawFboId_, readFboId_}) {
        auto fIt = fboId != 0 ? framebuffers_.find(fboId) : framebuffers_.end();
        if (fIt == framebuffers_.end()) continue;
        auto detach = [&](VkFboAttachment& att) {
            if (att.kind != VkFboAttachment::Kind::Texture || att.id != tex.id) return;
            framebufferChanged(fboId);
            att = {};
        };
        for (VkFboAttachment& c : fIt->second.color) detach(c);
        detach(fIt->second.depth);
        detach(fIt->second.stencil);
    }
    endRendering();
    releaseTexture(it->second);
    textures_.erase(it);
}

void WebGLVkContext::bindTexture(GLenum target, WebGLTexture tex) {
    if (activeTextureUnit_ >= boundTextures2D_.size()) return;
    if (target == kTextureCubeMap) {
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
        if (target == kTextureCubeMap) texId = boundTexturesCubeMap_[activeTextureUnit_];
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

// Give `tex` fresh storage: the previous image is released (destroyed once
// the GPU is done with it), the new one is cleared to zero — WebGL textures
// start initialised — and left sampleable.
bool WebGLVkContext::allocateTexture(VkTextureResource& tex, uint32_t width, uint32_t height, VkFormat format,
                                     uint32_t bpp, uint32_t mipLevels, uint32_t layers, bool cube) {
    // The new image and view are built before the old storage is released, so
    // a failed allocation leaves the texture as it was rather than half-defined.
    const bool depth = isDepthFormat(format);
    VkImageUsageFlags usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                              VK_IMAGE_USAGE_SAMPLED_BIT |
                              (depth ? VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT : VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT);
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkDeviceSize offset = 0;
    uint64_t allocId = 0;
    if (!context_.createImage(width, height, format, VK_IMAGE_TILING_OPTIMAL, usage,
                              VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, image, memory, offset, allocId,
                              mipLevels, layers, cube ? VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT : 0)) {
        LOG_ERROR("WebGLVkContext: Failed to allocate VkImage (%ux%u x%u)", width, height, layers);
        setSyntheticError(GL_OUT_OF_MEMORY);
        return false;
    }

    const VkImageSubresourceRange range{render::imageAspectFor(format), 0, mipLevels, 0, layers};
    VkImageViewCreateInfo viewInfo{};
    viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewInfo.image = image;
    viewInfo.viewType = cube ? VK_IMAGE_VIEW_TYPE_CUBE : layers > 1 || tex.target != GL_TEXTURE_2D
                                                         ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = format;
    // Sampling reads depth only; the view of a depth+stencil image says so.
    viewInfo.subresourceRange = range;
    if (depth) viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
    VkImageView view = VK_NULL_HANDLE;
    if (vkCreateImageView(context_.device(), &viewInfo, nullptr, &view) != VK_SUCCESS) {
        LOG_ERROR("WebGLVkContext: Failed to create image view (%ux%u)", width, height);
        context_.destroyImage(image, allocId);  // never referenced by a command
        setSyntheticError(GL_OUT_OF_MEMORY);
        return false;
    }

    // End any pass that has the old image attached before replacing it.
    VkCommandBuffer cmd = transferCommands();
    releaseTexture(tex);
    tex.image = image;
    tex.memory = memory;
    tex.offset = offset;
    tex.allocId = allocId;
    tex.view = view;
    tex.width = width;
    tex.height = height;
    tex.format = format;
    tex.bytesPerPixel = bpp;
    tex.mipLevels = mipLevels;
    tex.arrayLayers = layers;

    render::cmdTransitionImage(cmd, tex.image, range, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    if (depth) {
        VkClearDepthStencilValue clear{1.0f, 0};
        vkCmdClearDepthStencilImage(cmd, tex.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &clear, 1, &range);
    } else {
        VkClearColorValue clear{};
        vkCmdClearColorImage(cmd, tex.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &clear, 1, &range);
    }
    tex.currentLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    transitionTexture(cmd, tex, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    return true;
}

// Record a copy of client pixels into a region of `tex`: the pixels are
// unpacked into this frame's upload ring, copied in the command stream, and
// the texture is sampleable again after.
void WebGLVkContext::uploadTexture(VkTextureResource& tex, uint32_t level, uint32_t layer, uint32_t layerCount,
                                   int32_t x, int32_t y, uint32_t width, uint32_t height, uint32_t bpp,
                                   const void* pixels, bool unpack) {
    if (!tex.isValid() || !pixels || width == 0 || height == 0 || layerCount == 0) return;
    if (isDepthFormat(tex.format)) return;  // depth data uploads are not supported
    flushIfOverBudget();
    const VkDeviceSize layerBytes = static_cast<VkDeviceSize>(width) * height * bpp;
    render::UploadSlice staging = stage(nullptr, layerBytes * layerCount, std::max<VkDeviceSize>(bpp, 4));
    if (!staging) return;
    if (unpack) {
        copyAndProcessPixels(static_cast<uint8_t*>(staging.mapped), pixels, static_cast<GLsizei>(width),
                             static_cast<GLsizei>(height), bpp, unpackAlignment_, unpackFlipY_,
                             unpackPremultiplyAlpha_);
    } else {
        std::memcpy(staging.mapped, pixels, static_cast<size_t>(layerBytes * layerCount));
    }

    VkCommandBuffer cmd = transferCommands();
    transitionTexture(cmd, tex, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkBufferImageCopy region{};
    region.bufferOffset = staging.offset;
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, level, layer, layerCount};
    region.imageOffset = {x, y, 0};
    region.imageExtent = {width, height, 1};
    vkCmdCopyBufferToImage(cmd, staging.buffer, tex.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
    transitionTexture(cmd, tex, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
}

void WebGLVkContext::texStorage2D(GLenum target, GLsizei /*levels*/, GLenum internalformat,
                                  GLsizei width, GLsizei height) {
    texImage2D(target, 0, static_cast<GLint>(internalformat), width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
}

void WebGLVkContext::texImage2D(GLenum target, GLint level, GLint internalformat,
                                GLsizei width, GLsizei height, GLint /*border*/,
                                GLenum format, GLenum type, const void* pixels) {
    const bool cubeFace = isCubeFace(target);
    GLuint texId = 0;
    if (activeTextureUnit_ < boundTextures2D_.size()) {
        if (cubeFace || target == kTextureCubeMap) texId = boundTexturesCubeMap_[activeTextureUnit_];
        else texId = boundTextures2D_[activeTextureUnit_];
    }
    if (texId == 0 || width <= 0 || height <= 0 || level < 0) return;

    VkTextureResource& tex = textures_[texId];
    TexFormat fmt = chooseFormat(internalformat, format, type);
    if (isDepthFormat(fmt.format)) {
        // Depth textures share the device's depth formats with renderbuffers
        // and the canvas (webgl_vk_formats.h), so depth blits between them work.
        GLenum sized = static_cast<GLenum>(internalformat);
        if (sized == GL_DEPTH_COMPONENT)
            sized = type == GL_FLOAT ? GL_DEPTH_COMPONENT32F
                    : type == GL_UNSIGNED_SHORT ? GL_DEPTH_COMPONENT16 : GL_DEPTH_COMPONENT24;
        else if (sized == GL_DEPTH_STENCIL)
            sized = type == 0x8DAD /* FLOAT_32_UNSIGNED_INT_24_8_REV */ ? GL_DEPTH32F_STENCIL8 : GL_DEPTH24_STENCIL8;
        const VkFormat depth = depthStencilFormat(context_.physicalDevice(), sized);
        if (depth != VK_FORMAT_UNDEFINED) fmt.format = depth;
    }
    const uint32_t w = static_cast<uint32_t>(width);
    const uint32_t h = static_cast<uint32_t>(height);

    if (cubeFace) {
        tex.target = kTextureCubeMap;
        if (!tex.isValid() || tex.width != w || tex.height != h || tex.format != fmt.format) {
            if (!allocateTexture(tex, w, h, fmt.format, fmt.bpp, 1, 6, true)) return;
        }
        uploadTexture(tex, 0, target - kCubeFacePosX, 1, 0, 0, w, h, fmt.bpp, pixels, true);
        return;
    }

    // Level 0 defines the texture (and its full mip chain); the storage is
    // kept when the size and format do not change, so re-uploading a texture
    // every frame records a copy rather than recreating the image.
    tex.target = GL_TEXTURE_2D;
    const uint32_t numLevels = static_cast<uint32_t>(std::floor(std::log2(std::max(width, height)))) + 1;
    if (level == 0) {
        if (!tex.isValid() || tex.width != w || tex.height != h || tex.format != fmt.format ||
            tex.mipLevels != numLevels) {
            if (!allocateTexture(tex, w, h, fmt.format, fmt.bpp, numLevels, 1, false)) return;
        }
    } else if (!tex.isValid() || static_cast<uint32_t>(level) >= tex.mipLevels) {
        if (!tex.isValid()) setSyntheticError(GL_INVALID_OPERATION);
        return;
    }
    uploadTexture(tex, static_cast<uint32_t>(level), 0, 1, 0, 0, w, h, fmt.bpp, pixels, true);
}

void WebGLVkContext::texSubImage2D(GLenum target, GLint level, GLint xoffset, GLint yoffset,
                                   GLsizei width, GLsizei height,
                                   GLenum format, GLenum type, const void* pixels) {
    GLuint texId = 0;
    uint32_t layer = 0;
    if (activeTextureUnit_ < boundTextures2D_.size()) {
        if (isCubeFace(target)) {
            texId = boundTexturesCubeMap_[activeTextureUnit_];
            layer = target - kCubeFacePosX;
        } else {
            texId = boundTextures2D_[activeTextureUnit_];
        }
    }
    if (texId == 0 || !pixels || width <= 0 || height <= 0 || level < 0) return;

    auto it = textures_.find(texId);
    if (it == textures_.end() || !it->second.isValid()) return;
    VkTextureResource& tex = it->second;
    if (static_cast<uint32_t>(level) >= tex.mipLevels || xoffset < 0 || yoffset < 0) {
        setSyntheticError(GL_INVALID_VALUE);
        return;
    }
    uploadTexture(tex, static_cast<uint32_t>(level), layer, 1, xoffset, yoffset, static_cast<uint32_t>(width),
                  static_cast<uint32_t>(height), clientBpp(format, type, tex.bytesPerPixel), pixels, true);
}

void WebGLVkContext::generateMipmap(GLenum target) {
    GLuint texId = 0;
    if (activeTextureUnit_ < boundTextures2D_.size()) {
        if (target == kTextureCubeMap) texId = boundTexturesCubeMap_[activeTextureUnit_];
        else if (target == 0x8C1A) texId = boundTextures2DArray_[activeTextureUnit_];
        else texId = boundTextures2D_[activeTextureUnit_];
    }
    if (texId == 0) return;
    auto it = textures_.find(texId);
    if (it == textures_.end() || !it->second.isValid() || it->second.mipLevels <= 1) return;
    VkTextureResource& tex = it->second;
    if (isDepthFormat(tex.format)) return;

    VkFormatProperties props;
    vkGetPhysicalDeviceFormatProperties(context_.physicalDevice(), tex.format, &props);
    const VkFilter filter = (props.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT)
                                ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;

    // Level 0 becomes the blit source of level 1, each level the source of
    // the next, and the whole chain ends sampleable.
    VkCommandBuffer cmd = transferCommands();
    auto levelRange = [&](uint32_t base, uint32_t count) {
        return VkImageSubresourceRange{VK_IMAGE_ASPECT_COLOR_BIT, base, count, 0, tex.arrayLayers};
    };
    render::cmdTransitionImage(cmd, tex.image, levelRange(0, 1), tex.currentLayout,
                               VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    render::cmdTransitionImage(cmd, tex.image, levelRange(1, tex.mipLevels - 1), tex.currentLayout,
                               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    int32_t mipWidth = static_cast<int32_t>(tex.width);
    int32_t mipHeight = static_cast<int32_t>(tex.height);
    for (uint32_t i = 1; i < tex.mipLevels; ++i) {
        VkImageBlit blit{};
        blit.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, i - 1, 0, tex.arrayLayers};
        blit.srcOffsets[1] = {mipWidth, mipHeight, 1};
        mipWidth = std::max(1, mipWidth / 2);
        mipHeight = std::max(1, mipHeight / 2);
        blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, i, 0, tex.arrayLayers};
        blit.dstOffsets[1] = {mipWidth, mipHeight, 1};
        vkCmdBlitImage(cmd, tex.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                       tex.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, filter);
        if (i + 1 < tex.mipLevels) {
            render::cmdTransitionImage(cmd, tex.image, levelRange(i, 1), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                       VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        }
    }
    render::cmdTransitionImage(cmd, tex.image, levelRange(0, tex.mipLevels - 1),
                               VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    render::cmdTransitionImage(cmd, tex.image, levelRange(tex.mipLevels - 1, 1),
                               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    tex.currentLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
}

void WebGLVkContext::texImage3D(GLenum target, GLint /*level*/, GLint /*internalformat*/,
                                GLsizei width, GLsizei height, GLsizei depth, GLint /*border*/,
                                GLenum /*format*/, GLenum /*type*/, const void* pixels) {
    GLuint texId = (activeTextureUnit_ < boundTextures2DArray_.size()) ? boundTextures2DArray_[activeTextureUnit_] : 0;
    if (texId == 0 || width <= 0 || height <= 0 || depth <= 0) return;
    VkTextureResource& tex = textures_[texId];

    // TEXTURE_2D_ARRAY and TEXTURE_3D are both stored as a layered RGBA8 image.
    tex.target = target;
    tex.depth = static_cast<uint32_t>(depth);
    const uint32_t w = static_cast<uint32_t>(width);
    const uint32_t h = static_cast<uint32_t>(height);
    const uint32_t layers = static_cast<uint32_t>(depth);
    if (!tex.isValid() || tex.width != w || tex.height != h || tex.arrayLayers != layers ||
        tex.format != VK_FORMAT_R8G8B8A8_UNORM) {
        if (!allocateTexture(tex, w, h, VK_FORMAT_R8G8B8A8_UNORM, 4, 1, layers, false)) return;
    }
    uploadTexture(tex, 0, 0, layers, 0, 0, w, h, 4, pixels, false);
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
    if (level < 0 || static_cast<uint32_t>(level) >= tex.mipLevels || zoffset < 0 ||
        static_cast<uint32_t>(zoffset + depth) > tex.arrayLayers) {
        setSyntheticError(GL_INVALID_VALUE);
        return;
    }
    uploadTexture(tex, static_cast<uint32_t>(level), static_cast<uint32_t>(zoffset), static_cast<uint32_t>(depth),
                  xoffset, yoffset, static_cast<uint32_t>(width), static_cast<uint32_t>(height),
                  tex.bytesPerPixel, pixels, false);
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
