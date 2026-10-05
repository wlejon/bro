// Texture storage and uploads: texImage* / texSubImage* / texStorage* from
// client memory, a PIXEL_UNPACK_BUFFER or a decoded DOM source. Pixels are
// converted on the CPU (webgl_vk_pixels.h) into this frame's upload memory
// and copied into the image in the command stream.

#include "webgl/vulkan/webgl_vk_context.h"
#include "webgl/vulkan/webgl_vk_formats.h"
#include "render/vulkan_util.h"
#include "util/log.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace bro::webgl::vk {

namespace {

bool isCubeFace(GLenum target) {
    return target >= GL_TEXTURE_CUBE_MAP_POSITIVE_X && target <= GL_TEXTURE_CUBE_MAP_NEGATIVE_Z;
}

uint32_t faceOf(GLenum target) { return isCubeFace(target) ? target - GL_TEXTURE_CUBE_MAP_POSITIVE_X : 0; }

uint32_t fullChain(uint32_t width, uint32_t height, uint32_t depth) {
    return static_cast<uint32_t>(std::floor(std::log2(std::max({width, height, depth, 1u})))) + 1;
}

uint32_t levelSize(uint32_t base, uint32_t level) { return std::max(1u, base >> level); }

// Bytes of one component of a client type: a PBO offset must be a multiple.
uint32_t typeBytes(GLenum type) {
    switch (type) {
        case GL_UNSIGNED_BYTE: case GL_BYTE: return 1;
        case GL_UNSIGNED_SHORT: case GL_SHORT: case GL_HALF_FLOAT: case GL_UNSIGNED_SHORT_5_6_5:
        case GL_UNSIGNED_SHORT_4_4_4_4: case GL_UNSIGNED_SHORT_5_5_5_1:
            return 2;
        case GL_FLOAT_32_UNSIGNED_INT_24_8_REV: return 8;
        default: return 4;
    }
}

} // namespace

UnpackState WebGLVkContext::unpackState(const PixelSource& src) const {
    if (src.raw) {
        UnpackState state;
        state.alignment = 1;
        return state;
    }
    UnpackState state = unpack_;
    state.flipY = unpackFlipY_ && !src.pbo;
    state.premultiplyAlpha = unpackPremultiplyAlpha_ && !src.pbo;
    if (src.dom) {
        // A DOM source is its own image: UNPACK_ALIGNMENT and _ROW_LENGTH do
        // not apply, the skips select a sub-rectangle of it.
        state.alignment = 1;
        state.rowLength = static_cast<int32_t>(src.domWidth);
        if (state.imageHeight == 0) state.imageHeight = static_cast<int32_t>(src.domHeight);
    }
    return state;
}

bool WebGLVkContext::validateLevelSize(GLenum target, GLint level, GLsizei width, GLsizei height, GLsizei depth) {
    if (level < 0 || width < 0 || height < 0 || depth < 0) {
        setSyntheticError(GL_INVALID_VALUE);
        return false;
    }
    GLint maxSize = getParameterInt(GL_MAX_TEXTURE_SIZE);
    GLint maxDepth = 1;
    if (isCubeFace(target) || target == GL_TEXTURE_CUBE_MAP) {
        maxSize = getParameterInt(GL_MAX_CUBE_MAP_TEXTURE_SIZE);
    } else if (target == GL_TEXTURE_3D) {
        maxSize = maxDepth = getParameterInt(GL_MAX_3D_TEXTURE_SIZE);
    } else if (target == GL_TEXTURE_2D_ARRAY) {
        maxDepth = getParameterInt(GL_MAX_ARRAY_TEXTURE_LAYERS);
    }
    const GLint maxLevel = static_cast<GLint>(std::floor(std::log2(std::max(maxSize, 1))));
    const GLint levelMax = level <= maxLevel ? maxSize >> level : 0;
    const GLint levelMaxDepth = target == GL_TEXTURE_3D ? levelMax : maxDepth;
    if (level > maxLevel || width > levelMax || height > levelMax || depth > levelMaxDepth ||
        ((isCubeFace(target) || target == GL_TEXTURE_CUBE_MAP) && width != height)) {
        setSyntheticError(GL_INVALID_VALUE);
        return false;
    }
    return true;
}

// Storage for `tex` as `tf`: a 2D, cube-compatible, layered or 3D image
// with the levels asked for, every subresource sampleable. Levels of the old
// image defined at the size and format the new one gives them are copied
// across; the rest are dropped (they could not be complete with the new
// image anyway). A failed allocation leaves the texture as it was.
bool WebGLVkContext::allocateTexture(VkTextureResource& tex, const TexFormat& tf, uint32_t width, uint32_t height,
                                     uint32_t depth, uint32_t levels) {
    const bool cube = tex.target == GL_TEXTURE_CUBE_MAP;
    const bool is3D = tex.is3D();
    const bool array = tex.target == GL_TEXTURE_2D_ARRAY;
    VkImageCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    info.imageType = is3D ? VK_IMAGE_TYPE_3D : VK_IMAGE_TYPE_2D;
    info.format = tf.format;
    info.extent = {width, height, is3D ? depth : 1};
    info.mipLevels = levels ? levels : fullChain(width, height, is3D ? depth : 1);
    info.arrayLayers = cube ? 6 : array ? depth : 1;
    info.samples = VK_SAMPLE_COUNT_1_BIT;
    info.tiling = VK_IMAGE_TILING_OPTIMAL;
    info.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    // Renderable when the device can render the format: framebufferTexture
    // attaches any level (a 3D texture's slices through 2D views).
    if (tf.isDepthOrStencil()) {
        if (!is3D && formatSupports(tf.format, VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT))
            info.usage |= VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
    } else if (!tf.compressed && formatSupports(tf.format, VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT)) {
        info.usage |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
        if (is3D) info.flags |= VK_IMAGE_CREATE_2D_ARRAY_COMPATIBLE_BIT;
    }
    if (cube) info.flags |= VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;

    VkTextureResource next;
    if (!context_.createImage(info, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, next.image, next.memory, next.offset,
                              next.allocId)) {
        LOG_ERROR("WebGLVkContext: failed to allocate a %ux%ux%u texture", width, height, depth);
        setSyntheticError(GL_OUT_OF_MEMORY);
        return false;
    }
    next.tf = tf;
    next.format = tf.format;
    next.width = width;
    next.height = height;
    next.depth = is3D ? depth : 1;
    next.mipLevels = info.mipLevels;
    next.arrayLayers = info.arrayLayers;
    next.layouts.assign(static_cast<size_t>(next.mipLevels) * next.arrayLayers, VK_IMAGE_LAYOUT_UNDEFINED);
    next.target = tex.target;

    // A pass may have the old image attached; it is destroyed once the GPU is done.
    VkCommandBuffer cmd = transferCommands();
    std::vector<TexLevel> kept(next.mipLevels);
    const VkImageAspectFlags aspects = render::imageAspectFor(tf.format);
    for (uint32_t l = 0; tex.isValid() && tex.format == tf.format && l < tex.levels.size(); ++l) {
        const TexLevel& lv = tex.levels[l];
        if (lv.faces == 0 || l >= next.mipLevels || l >= tex.mipLevels || lv.width != levelSize(width, l) ||
            lv.height != levelSize(height, l) || lv.depth != (is3D ? levelSize(depth, l) : (array ? depth : 1)) ||
            lv.format.internalformat != tf.internalformat)
            continue;
        transitionTextureRange(cmd, tex, l, 1, 0, tex.arrayLayers, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        transitionTextureRange(cmd, next, l, 1, 0, next.arrayLayers, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        VkImageCopy region{};
        region.srcSubresource = {aspects, l, 0, next.arrayLayers};
        region.dstSubresource = region.srcSubresource;
        region.extent = {lv.width, lv.height, is3D ? lv.depth : 1};
        vkCmdCopyImage(cmd, tex.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, next.image,
                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
        kept[l] = lv;
    }
    transitionTexture(cmd, next, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

    releaseTexture(tex);
    tex.image = next.image;
    tex.memory = next.memory;
    tex.offset = next.offset;
    tex.allocId = next.allocId;
    tex.tf = next.tf;
    tex.format = next.format;
    tex.width = next.width;
    tex.height = next.height;
    tex.depth = next.depth;
    tex.mipLevels = next.mipLevels;
    tex.arrayLayers = next.arrayLayers;
    tex.layouts = std::move(next.layouts);
    tex.levels = std::move(kept);
    return true;
}

bool WebGLVkContext::defineLevel(VkTextureResource& tex, uint32_t level, uint32_t face, const TexFormat& tf,
                                 uint32_t width, uint32_t height, uint32_t depth) {
    const bool is3D = tex.is3D();
    const bool array = tex.target == GL_TEXTURE_2D_ARRAY;
    const bool fits = tex.isValid() && tex.format == tf.format && tex.tf.internalformat == tf.internalformat &&
                      level < tex.mipLevels && levelSize(tex.width, level) == width &&
                      levelSize(tex.height, level) == height &&
                      (is3D ? levelSize(tex.depth, level) == depth : !array || tex.arrayLayers == depth);
    if (!fits) {
        // The chain this level is a level of. Level 0 decides when it is
        // already defined at this level's chain; otherwise this level does.
        const uint32_t w = std::max(1u, width << level), h = std::max(1u, height << level);
        const uint32_t d = is3D ? std::max(1u, depth << level) : depth;
        if (!allocateTexture(tex, tf, w, h, d, 0)) return false;
    }
    if (tex.levels.size() < tex.mipLevels) tex.levels.resize(tex.mipLevels);
    TexLevel& lv = tex.levels[level];
    const bool same = lv.width == width && lv.height == height && lv.depth == depth &&
                      lv.format.internalformat == tf.internalformat && lv.format.format == tf.format;
    lv.faces = static_cast<uint8_t>((same ? lv.faces : 0) | (1u << face));
    lv.width = width;
    lv.height = height;
    lv.depth = depth;
    lv.format = tf;
    return true;
}

// The bound PIXEL_UNPACK_BUFFER from byte `offset` on, as an upload source
// of src.type (0 for compressed data, which has no alignment to keep).
bool WebGLVkContext::pboSource(GLintptr offset, PixelSource& src) {
    auto it = boundPixelUnpackBuffer_ != 0 ? buffers_.find(boundPixelUnpackBuffer_) : buffers_.end();
    if (it == buffers_.end() || it->second.isMapped || unpackFlipY_ || unpackPremultiplyAlpha_ || offset < 0 ||
        static_cast<size_t>(offset) > it->second.shadowData.size() ||
        (src.type != 0 && offset % typeBytes(src.type) != 0)) {
        setSyntheticError(GL_INVALID_OPERATION);
        return false;
    }
    syncShadow(it->second);
    src.data = it->second.shadowData.data() + offset;
    src.size = it->second.shadowData.size() - static_cast<size_t>(offset);
    src.pbo = true;
    return true;
}

void WebGLVkContext::zeroTexels(VkTextureResource& tex, uint32_t level, uint32_t layer, uint32_t layerCount) {
    if (!tex.isValid()) return;
    VkCommandBuffer cmd = transferCommands();
    transitionTextureRange(cmd, tex, level, 1, layer, layerCount, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    const VkImageSubresourceRange range{render::imageAspectFor(tex.format), level, 1, layer, layerCount};
    if (tex.tf.compressed) {
        // Compressed images are not cleared, but copied from zero blocks.
        const uint32_t w = levelSize(tex.width, level), h = levelSize(tex.height, level);
        const uint32_t d = tex.is3D() ? levelSize(tex.depth, level) : 1;
        const size_t bytes = compressedImageSize(tex.tf, w, h, d) * layerCount;
        flushIfOverBudget();
        cmd = transferCommands();
        render::UploadSlice zeros = stage(nullptr, bytes, 16);
        if (!zeros) return;
        VkBufferImageCopy region{};
        region.bufferOffset = zeros.offset;
        region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, level, layer, layerCount};
        region.imageExtent = {w, h, d};
        vkCmdCopyBufferToImage(cmd, zeros.buffer, tex.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
    } else if (range.aspectMask == VK_IMAGE_ASPECT_COLOR_BIT) {
        const VkClearColorValue zero{};
        vkCmdClearColorImage(cmd, tex.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &zero, 1, &range);
    } else {
        const VkClearDepthStencilValue zero{0.0f, 0};
        vkCmdClearDepthStencilImage(cmd, tex.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &zero, 1, &range);
    }
    transitionTextureRange(cmd, tex, level, 1, layer, layerCount, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
}

void WebGLVkContext::writeTexels(VkTextureResource& tex, uint32_t level, int32_t x, int32_t y, int32_t z,
                                 uint32_t width, uint32_t height, uint32_t depth, const PixelSource& src) {
    if (!tex.isValid() || width == 0 || height == 0 || depth == 0) return;
    flushIfOverBudget();
    const TexFormat& tf = tex.tf;
    const bool depthStencil = tf.isDepthOrStencil();
    const bool stencil = tf.kind == TexKind::DepthStencil;
    const uint32_t texel = depthStencil ? storageTexelSize(tf.format, VK_IMAGE_ASPECT_DEPTH_BIT)
                                        : colorTexelSize(tf.format);
    const VkDeviceSize count = static_cast<VkDeviceSize>(width) * height * depth;
    render::UploadSlice texels = stage(nullptr, count * texel, 16);
    render::UploadSlice stencils = stencil ? stage(nullptr, count, 16) : render::UploadSlice{};
    if (!texels || (stencil && !stencils)) return;
    unpackPixels(unpackState(src), src.data, src.format, src.type, width, height, depth, tf,
                 static_cast<uint8_t*>(texels.mapped), stencil ? static_cast<uint8_t*>(stencils.mapped) : nullptr);

    const bool is3D = tex.is3D();
    const uint32_t layer = is3D ? 0 : static_cast<uint32_t>(z), layers = is3D ? 1 : depth;
    VkCommandBuffer cmd = transferCommands();
    transitionTextureRange(cmd, tex, level, 1, layer, layers, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkBufferImageCopy regions[2]{};
    regions[0].bufferOffset = texels.offset;
    regions[0].imageSubresource = {depthStencil ? VkImageAspectFlags(VK_IMAGE_ASPECT_DEPTH_BIT)
                                                : VkImageAspectFlags(VK_IMAGE_ASPECT_COLOR_BIT),
                                   level, layer, layers};
    regions[0].imageOffset = {x, y, is3D ? z : 0};
    regions[0].imageExtent = {width, height, is3D ? depth : 1};
    regions[1] = regions[0];
    regions[1].bufferOffset = stencils.offset;
    regions[1].imageSubresource.aspectMask = VK_IMAGE_ASPECT_STENCIL_BIT;
    vkCmdCopyBufferToImage(cmd, texels.buffer, tex.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &regions[0]);
    if (stencil)
        vkCmdCopyBufferToImage(cmd, stencils.buffer, tex.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
                               &regions[1]);
    transitionTextureRange(cmd, tex, level, 1, layer, layers, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
}

// ---------------------------------------------------------------------------
// texImage* / texSubImage*
// ---------------------------------------------------------------------------

void WebGLVkContext::texImage(GLenum target, GLint level, GLint internalformat, GLsizei width, GLsizei height,
                              GLsizei depth, GLint border, const PixelSource& src, bool is3D) {
    VkTextureResource* tex = textureForTarget(target, is3D ? k3DTargets : k2DTargets);
    if (!tex || !validateLevelSize(target, level, width, height, depth)) return;
    if (border != 0) {
        setSyntheticError(GL_INVALID_VALUE);
        return;
    }
    TexFormat tf;
    GLenum error = GL_NO_ERROR;
    if (!resolveTexFormat(context_.physicalDevice(), internalformat, src.format, src.type, tf, error)) {
        setSyntheticError(error);
        return;
    }
    if (tex->immutable || (tf.isDepthOrStencil() && (target == GL_TEXTURE_3D || src.dom))) {
        setSyntheticError(GL_INVALID_OPERATION);
        return;
    }
    if (src.data && unpackedSize(unpackState(src), src.format, src.type, static_cast<uint32_t>(width),
                                 static_cast<uint32_t>(height), static_cast<uint32_t>(depth)) > src.size) {
        setSyntheticError(GL_INVALID_OPERATION);
        return;
    }
    const auto l = static_cast<uint32_t>(level), w = static_cast<uint32_t>(width),
               h = static_cast<uint32_t>(height), d = static_cast<uint32_t>(depth);
    const uint32_t face = faceOf(target);
    if (w == 0 || h == 0 || d == 0) {
        // A zero-sized image: defined, and makes the texture incomplete.
        if (tex->levels.size() <= l) tex->levels.resize(l + 1);
        tex->levels[l] = TexLevel{w, h, d, static_cast<uint8_t>(1u << face), tf};
        return;
    }
    if (!defineLevel(*tex, l, face, tf, w, h, d)) return;
    const bool array = target == GL_TEXTURE_2D_ARRAY;
    if (src.data) writeTexels(*tex, l, 0, 0, static_cast<int32_t>(face), w, h, d, src);
    else zeroTexels(*tex, l, face, array ? d : 1);
}

void WebGLVkContext::texSubImage(GLenum target, GLint level, GLint xoffset, GLint yoffset, GLint zoffset,
                                 GLsizei width, GLsizei height, GLsizei depth, const PixelSource& src, bool is3D) {
    VkTextureResource* tex = textureForTarget(target, is3D ? k3DTargets : k2DTargets);
    if (!tex) return;
    if (level < 0 || xoffset < 0 || yoffset < 0 || zoffset < 0 || width < 0 || height < 0 || depth < 0) {
        setSyntheticError(GL_INVALID_VALUE);
        return;
    }
    const uint32_t face = faceOf(target);
    const auto l = static_cast<uint32_t>(level);
    if (l >= tex->levels.size() || !(tex->levels[l].faces & (1u << face))) {
        setSyntheticError(GL_INVALID_OPERATION);
        return;
    }
    const TexLevel& lv = tex->levels[l];
    if (static_cast<int64_t>(xoffset) + width > lv.width || static_cast<int64_t>(yoffset) + height > lv.height ||
        static_cast<int64_t>(zoffset) + depth > lv.depth) {
        setSyntheticError(GL_INVALID_VALUE);
        return;
    }
    if (lv.format.compressed || (src.dom && lv.format.isDepthOrStencil()) ||
        !clientFormatCompatible(context_.physicalDevice(), lv.format, src.format, src.type)) {
        setSyntheticError(clientPixelSize(src.format, src.type) == 0 ? GL_INVALID_ENUM : GL_INVALID_OPERATION);
        return;
    }
    if (!src.data) {
        setSyntheticError(GL_INVALID_VALUE);
        return;
    }
    if (unpackedSize(unpackState(src), src.format, src.type, static_cast<uint32_t>(width),
                     static_cast<uint32_t>(height), static_cast<uint32_t>(depth)) > src.size) {
        setSyntheticError(GL_INVALID_OPERATION);
        return;
    }
    writeTexels(*tex, l, xoffset, yoffset, is3D ? zoffset : static_cast<int32_t>(face), static_cast<uint32_t>(width),
                static_cast<uint32_t>(height), static_cast<uint32_t>(depth), src);
}

void WebGLVkContext::texImage2D(GLenum target, GLint level, GLint internalformat, GLsizei width, GLsizei height,
                                GLint border, GLenum format, GLenum type, const void* pixels, size_t size) {
    // With a PIXEL_UNPACK_BUFFER bound only a null (allocate-only) upload
    // takes client memory.
    if (boundPixelUnpackBuffer_ != 0 && pixels) {
        setSyntheticError(GL_INVALID_OPERATION);
        return;
    }
    texImage(target, level, internalformat, width, height, 1, border, {pixels, size, format, type}, false);
}

void WebGLVkContext::texSubImage2D(GLenum target, GLint level, GLint xoffset, GLint yoffset, GLsizei width,
                                   GLsizei height, GLenum format, GLenum type, const void* pixels, size_t size) {
    if (boundPixelUnpackBuffer_ != 0) {
        setSyntheticError(GL_INVALID_OPERATION);
        return;
    }
    texSubImage(target, level, xoffset, yoffset, 0, width, height, 1, {pixels, size, format, type}, false);
}

void WebGLVkContext::texImage3D(GLenum target, GLint level, GLint internalformat, GLsizei width, GLsizei height,
                                GLsizei depth, GLint border, GLenum format, GLenum type, const void* pixels,
                                size_t size) {
    // With a PIXEL_UNPACK_BUFFER bound only a null (allocate-only) upload
    // takes client memory.
    if (boundPixelUnpackBuffer_ != 0 && pixels) {
        setSyntheticError(GL_INVALID_OPERATION);
        return;
    }
    texImage(target, level, internalformat, width, height, depth, border, {pixels, size, format, type}, true);
}

void WebGLVkContext::texSubImage3D(GLenum target, GLint level, GLint xoffset, GLint yoffset, GLint zoffset,
                                   GLsizei width, GLsizei height, GLsizei depth, GLenum format, GLenum type,
                                   const void* pixels, size_t size) {
    if (boundPixelUnpackBuffer_ != 0) {
        setSyntheticError(GL_INVALID_OPERATION);
        return;
    }
    texSubImage(target, level, xoffset, yoffset, zoffset, width, height, depth, {pixels, size, format, type}, true);
}

void WebGLVkContext::texImage2DSource(GLenum target, GLint level, GLint internalformat, GLsizei width,
                                      GLsizei height, GLenum format, GLenum type, const uint8_t* rgba,
                                      uint32_t srcWidth, uint32_t srcHeight) {
    if (boundPixelUnpackBuffer_ != 0) {
        setSyntheticError(GL_INVALID_OPERATION);
        return;
    }
    PixelSource src{rgba, static_cast<size_t>(srcWidth) * srcHeight * 4, GL_RGBA, GL_UNSIGNED_BYTE, true,
                    srcWidth, srcHeight};
    // The source's data is RGBA8 whatever format / type say; they choose the
    // internal format and are validated as such.
    TexFormat tf;
    GLenum error = GL_NO_ERROR;
    if (!resolveTexFormat(context_.physicalDevice(), internalformat, format, type, tf, error)) {
        setSyntheticError(error);
        return;
    }
    if (width < 0) width = static_cast<GLsizei>(srcWidth);
    if (height < 0) height = static_cast<GLsizei>(srcHeight);
    // Through texImage with the client type resolved above standing in.
    VkTextureResource* tex = textureForTarget(target, k2DTargets);
    if (!tex || !validateLevelSize(target, level, width, height, 1)) return;
    if (tex->immutable || tf.isDepthOrStencil()) {
        setSyntheticError(GL_INVALID_OPERATION);
        return;
    }
    if (unpackedSize(unpackState(src), GL_RGBA, GL_UNSIGNED_BYTE, static_cast<uint32_t>(width),
                     static_cast<uint32_t>(height), 1) > src.size) {
        setSyntheticError(GL_INVALID_OPERATION);
        return;
    }
    const uint32_t face = faceOf(target);
    if (width == 0 || height == 0) return;
    if (!defineLevel(*tex, static_cast<uint32_t>(level), face, tf, static_cast<uint32_t>(width),
                     static_cast<uint32_t>(height), 1))
        return;
    writeTexels(*tex, static_cast<uint32_t>(level), 0, 0, static_cast<int32_t>(face), static_cast<uint32_t>(width),
                static_cast<uint32_t>(height), 1, src);
}

void WebGLVkContext::texSubImage2DSource(GLenum target, GLint level, GLint xoffset, GLint yoffset, GLsizei width,
                                         GLsizei height, GLenum format, GLenum type, const uint8_t* rgba,
                                         uint32_t srcWidth, uint32_t srcHeight) {
    if (boundPixelUnpackBuffer_ != 0) {
        setSyntheticError(GL_INVALID_OPERATION);
        return;
    }
    VkTextureResource* tex = textureForTarget(target, k2DTargets);
    if (!tex) return;
    const auto l = static_cast<uint32_t>(std::max(level, 0));
    if (level < 0 || l >= tex->levels.size() ||
        !clientFormatCompatible(context_.physicalDevice(), tex->levels[l].format, format, type)) {
        setSyntheticError(level < 0 ? GL_INVALID_VALUE : GL_INVALID_OPERATION);
        return;
    }
    if (width < 0) width = static_cast<GLsizei>(srcWidth);
    if (height < 0) height = static_cast<GLsizei>(srcHeight);
    // The format check is done; the data itself is RGBA8.
    PixelSource src{rgba, static_cast<size_t>(srcWidth) * srcHeight * 4, GL_RGBA, GL_UNSIGNED_BYTE, true,
                    srcWidth, srcHeight};
    const TexLevel& lv = tex->levels[l];
    if (!(lv.faces & (1u << faceOf(target))) || lv.format.isDepthOrStencil()) {
        setSyntheticError(GL_INVALID_OPERATION);
        return;
    }
    if (xoffset < 0 || yoffset < 0 || width < 0 || height < 0 ||
        static_cast<int64_t>(xoffset) + width > lv.width || static_cast<int64_t>(yoffset) + height > lv.height) {
        setSyntheticError(GL_INVALID_VALUE);
        return;
    }
    if (unpackedSize(unpackState(src), GL_RGBA, GL_UNSIGNED_BYTE, static_cast<uint32_t>(width),
                     static_cast<uint32_t>(height), 1) > src.size) {
        setSyntheticError(GL_INVALID_OPERATION);
        return;
    }
    writeTexels(*tex, l, xoffset, yoffset, static_cast<int32_t>(faceOf(target)), static_cast<uint32_t>(width),
                static_cast<uint32_t>(height), 1, src);
}

void WebGLVkContext::texImage2DFromPBO(GLenum target, GLint level, GLint internalformat, GLsizei width,
                                       GLsizei height, GLint border, GLenum format, GLenum type, GLintptr offset) {
    PixelSource src{nullptr, 0, format, type};
    if (pboSource(offset, src)) texImage(target, level, internalformat, width, height, 1, border, src, false);
}

void WebGLVkContext::texSubImage2DFromPBO(GLenum target, GLint level, GLint xoffset, GLint yoffset,
                                          GLsizei width, GLsizei height, GLenum format, GLenum type,
                                          GLintptr offset) {
    PixelSource src{nullptr, 0, format, type};
    if (pboSource(offset, src)) texSubImage(target, level, xoffset, yoffset, 0, width, height, 1, src, false);
}

void WebGLVkContext::texImage3DFromPBO(GLenum target, GLint level, GLint internalformat, GLsizei width,
                                       GLsizei height, GLsizei depth, GLint border, GLenum format, GLenum type,
                                       GLintptr offset) {
    PixelSource src{nullptr, 0, format, type};
    if (pboSource(offset, src)) texImage(target, level, internalformat, width, height, depth, border, src, true);
}

void WebGLVkContext::texSubImage3DFromPBO(GLenum target, GLint level, GLint xoffset, GLint yoffset, GLint zoffset,
                                          GLsizei width, GLsizei height, GLsizei depth, GLenum format, GLenum type,
                                          GLintptr offset) {
    PixelSource src{nullptr, 0, format, type};
    if (pboSource(offset, src))
        texSubImage(target, level, xoffset, yoffset, zoffset, width, height, depth, src, true);
}

// ---------------------------------------------------------------------------
// texStorage*
// ---------------------------------------------------------------------------

// Immutable storage: every level defined (as zeros) and fixed; texImage*
// on it is INVALID_OPERATION, texSubImage* fills it.
static bool storageTarget(GLenum target, bool is3D) {
    return is3D ? target == GL_TEXTURE_3D || target == GL_TEXTURE_2D_ARRAY
                : target == GL_TEXTURE_2D || target == GL_TEXTURE_CUBE_MAP;
}

void WebGLVkContext::texStorage2D(GLenum target, GLsizei levels, GLenum internalformat, GLsizei width,
                                  GLsizei height) {
    texStorage3D(target, levels, internalformat, width, height, 1);
}

void WebGLVkContext::texStorage3D(GLenum target, GLsizei levels, GLenum internalformat, GLsizei width,
                                  GLsizei height, GLsizei depth) {
    const bool is3D = target == GL_TEXTURE_3D || target == GL_TEXTURE_2D_ARRAY;
    if (!storageTarget(target, is3D)) {
        setSyntheticError(GL_INVALID_ENUM);
        return;
    }
    VkTextureResource* tex = textureForTarget(target, kTextureTargets);
    if (!tex) return;
    if (levels < 1 || width < 1 || height < 1 || depth < 1) {
        setSyntheticError(GL_INVALID_VALUE);
        return;
    }
    TexFormat tf;
    const bool compressed = resolveCompressedFormat(internalformat, tf);
    if (compressed ? !compressedFormatEnabled(internalformat)
                   : !resolveSizedFormat(context_.physicalDevice(), internalformat, tf)) {
        setSyntheticError(GL_INVALID_ENUM);
        return;
    }
    if (!validateLevelSize(target, 0, width, height, depth)) return;
    const uint32_t w = static_cast<uint32_t>(width), h = static_cast<uint32_t>(height),
                   d = static_cast<uint32_t>(depth);
    if (tex->immutable || static_cast<uint32_t>(levels) > fullChain(w, h, target == GL_TEXTURE_3D ? d : 1) ||
        (target == GL_TEXTURE_3D && (tf.isDepthOrStencil() || compressed))) {
        setSyntheticError(GL_INVALID_OPERATION);
        return;
    }
    tex->levels.clear();
    if (!allocateTexture(*tex, tf, w, h, d, static_cast<uint32_t>(levels))) return;
    const bool cube = target == GL_TEXTURE_CUBE_MAP;
    for (uint32_t l = 0; l < tex->mipLevels; ++l) {
        TexLevel& lv = tex->levels[l];
        lv = TexLevel{levelSize(w, l), levelSize(h, l), target == GL_TEXTURE_3D ? levelSize(d, l) : d,
                      static_cast<uint8_t>(cube ? 0x3F : 0x01), tf};
        zeroTexels(*tex, l, 0, tex->arrayLayers);
    }
    tex->immutable = true;
    tex->immutableLevels = static_cast<uint32_t>(levels);
}

} // namespace bro::webgl::vk
