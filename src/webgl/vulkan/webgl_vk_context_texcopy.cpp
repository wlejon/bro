// Compressed images (stored as they come, in the formats the device
// samples natively), copies from the read framebuffer into a texture
// (copyTex*: an image copy or blit on the GPU), and generateMipmap (a blit
// chain down the levels).

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

bool isBC(VkFormat format) { return format >= VK_FORMAT_BC1_RGB_UNORM_BLOCK && format <= VK_FORMAT_BC7_SRGB_BLOCK; }

bool isSrgb(VkFormat format) {
    return format == VK_FORMAT_R8G8B8A8_SRGB || format == VK_FORMAT_B8G8R8A8_SRGB;
}

// Fixed-point (normalized) color: what ES 3.0 calls a fixed-point buffer.
bool isFixedPoint(VkFormat format) {
    const FormatBits bits = formatBits(format);
    return !isIntegerFormat(format) && (bits.red == 8 || bits.red == 10 || bits.red == 5 || bits.red == 4);
}

} // namespace

// ---------------------------------------------------------------------------
// Compressed images
// ---------------------------------------------------------------------------

std::vector<std::string> WebGLVkContext::compressedTextureExtensions() const {
    std::vector<std::string> names;
    const CompressedExtension* exts = nullptr;
    const size_t n = compressedExtensions(exts);
    for (size_t i = 0; i < n; ++i)
        if (compressedExtensionSupported(context_.physicalDevice(), context_.features(), exts[i]))
            names.emplace_back(exts[i].name);
    return names;
}

bool WebGLVkContext::enableCompressedExtension(const std::string& name) {
    for (const std::string& ext : compressedTextureExtensions()) {
        if (ext != name) continue;
        if (std::find(enabledCompressedExtensions_.begin(), enabledCompressedExtensions_.end(), name) ==
            enabledCompressedExtensions_.end())
            enabledCompressedExtensions_.push_back(name);
        return true;
    }
    return false;
}

std::vector<GLint> WebGLVkContext::compressedTextureFormats() const {
    std::vector<GLint> formats;
    const CompressedExtension* exts = nullptr;
    const size_t n = compressedExtensions(exts);
    for (size_t i = 0; i < n; ++i) {
        if (std::find(enabledCompressedExtensions_.begin(), enabledCompressedExtensions_.end(), exts[i].name) ==
            enabledCompressedExtensions_.end())
            continue;
        for (size_t f = 0; f < exts[i].count; ++f)
            if (std::find(formats.begin(), formats.end(), static_cast<GLint>(exts[i].formats[f])) == formats.end())
                formats.push_back(static_cast<GLint>(exts[i].formats[f]));
    }
    return formats;
}

bool WebGLVkContext::compressedFormatEnabled(GLenum internalformat) const {
    const std::vector<GLint> formats = compressedTextureFormats();
    return std::find(formats.begin(), formats.end(), static_cast<GLint>(internalformat)) != formats.end();
}

bool WebGLVkContext::writeCompressed(VkTextureResource& tex, uint32_t level, int32_t x, int32_t y, int32_t z,
                                     uint32_t width, uint32_t height, uint32_t depth, const void* data,
                                     size_t size) {
    flushIfOverBudget();
    render::UploadSlice slice = stage(data, size, 16);
    if (!slice) return false;
    VkCommandBuffer cmd = transferCommands();
    transitionTextureRange(cmd, tex, level, 1, static_cast<uint32_t>(z), depth, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkBufferImageCopy region{};
    region.bufferOffset = slice.offset;
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, level, static_cast<uint32_t>(z), depth};
    region.imageOffset = {x, y, 0};
    region.imageExtent = {width, height, 1};
    vkCmdCopyBufferToImage(cmd, slice.buffer, tex.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
    transitionTextureRange(cmd, tex, level, 1, static_cast<uint32_t>(z), depth,
                           VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    return true;
}

void WebGLVkContext::compressedImage(GLenum target, GLint level, GLenum internalformat, GLsizei width,
                                     GLsizei height, GLsizei depth, GLint border, const void* data, size_t size,
                                     bool is3D) {
    VkTextureResource* tex = textureForTarget(target, is3D ? k3DTargets : k2DTargets);
    if (!tex) return;
    TexFormat tf;
    if (!resolveCompressedFormat(internalformat, tf) || !compressedFormatEnabled(internalformat)) {
        setSyntheticError(GL_INVALID_ENUM);
        return;
    }
    if (!validateLevelSize(target, level, width, height, depth)) return;
    if (border != 0) {
        setSyntheticError(GL_INVALID_VALUE);
        return;
    }
    const auto w = static_cast<uint32_t>(width), h = static_cast<uint32_t>(height),
               d = static_cast<uint32_t>(depth);
    if (size != compressedImageSize(tf, w, h, d)) {
        setSyntheticError(GL_INVALID_VALUE);
        return;
    }
    // Block-compressed (S3TC, RGTC, BPTC) images are whole blocks, but for
    // the small levels of a chain; no compressed format is 3D here.
    auto blockFit = [&](uint32_t v) { return level == 0 ? v % 4 == 0 : v <= 2 || v % 4 == 0; };
    if (tex->immutable || target == GL_TEXTURE_3D || (isBC(tf.format) && (!blockFit(w) || !blockFit(h)))) {
        setSyntheticError(GL_INVALID_OPERATION);
        return;
    }
    if (w == 0 || h == 0 || d == 0) return;
    const uint32_t face = faceOf(target);
    if (!defineLevel(*tex, static_cast<uint32_t>(level), face, tf, w, h, d)) return;
    writeCompressed(*tex, static_cast<uint32_t>(level), 0, 0, static_cast<int32_t>(face), w, h,
                    target == GL_TEXTURE_2D_ARRAY ? d : 1, data, size);
}

void WebGLVkContext::compressedSubImage(GLenum target, GLint level, GLint xoffset, GLint yoffset, GLint zoffset,
                                        GLsizei width, GLsizei height, GLsizei depth, GLenum format,
                                        const void* data, size_t size, bool is3D) {
    VkTextureResource* tex = textureForTarget(target, is3D ? k3DTargets : k2DTargets);
    if (!tex) return;
    TexFormat tf;
    if (!resolveCompressedFormat(format, tf) || !compressedFormatEnabled(format)) {
        setSyntheticError(GL_INVALID_ENUM);
        return;
    }
    if (level < 0 || xoffset < 0 || yoffset < 0 || zoffset < 0 || width < 0 || height < 0 || depth < 0) {
        setSyntheticError(GL_INVALID_VALUE);
        return;
    }
    const auto l = static_cast<uint32_t>(level);
    const uint32_t face = faceOf(target);
    if (l >= tex->levels.size() || !(tex->levels[l].faces & (1u << face)) ||
        tex->levels[l].format.internalformat != format) {
        setSyntheticError(GL_INVALID_OPERATION);
        return;
    }
    const TexLevel& lv = tex->levels[l];
    if (static_cast<int64_t>(xoffset) + width > lv.width || static_cast<int64_t>(yoffset) + height > lv.height ||
        static_cast<int64_t>(zoffset) + depth > lv.depth) {
        setSyntheticError(GL_INVALID_VALUE);
        return;
    }
    // Whole blocks, but where the region ends at the level's edge.
    const auto w = static_cast<uint32_t>(width), h = static_cast<uint32_t>(height),
               d = static_cast<uint32_t>(depth);
    if (xoffset % tf.blockWidth != 0 || yoffset % tf.blockHeight != 0 ||
        (w % tf.blockWidth != 0 && xoffset + w != lv.width) || (h % tf.blockHeight != 0 && yoffset + h != lv.height)) {
        setSyntheticError(GL_INVALID_OPERATION);
        return;
    }
    if (size != compressedImageSize(tf, w, h, d)) {
        setSyntheticError(GL_INVALID_VALUE);
        return;
    }
    if (w == 0 || h == 0 || d == 0) return;
    writeCompressed(*tex, l, xoffset, yoffset, is3D ? zoffset : static_cast<int32_t>(face), w, h, d, data, size);
}

void WebGLVkContext::compressedTexImage2D(GLenum target, GLint level, GLenum internalformat, GLsizei width,
                                          GLsizei height, GLint border, const void* data, size_t size) {
    if (boundPixelUnpackBuffer_ != 0) {
        setSyntheticError(GL_INVALID_OPERATION);
        return;
    }
    compressedImage(target, level, internalformat, width, height, 1, border, data, size, false);
}

void WebGLVkContext::compressedTexSubImage2D(GLenum target, GLint level, GLint xoffset, GLint yoffset,
                                             GLsizei width, GLsizei height, GLenum format, const void* data,
                                             size_t size) {
    if (boundPixelUnpackBuffer_ != 0) {
        setSyntheticError(GL_INVALID_OPERATION);
        return;
    }
    compressedSubImage(target, level, xoffset, yoffset, 0, width, height, 1, format, data, size, false);
}

void WebGLVkContext::compressedTexImage3D(GLenum target, GLint level, GLenum internalformat, GLsizei width,
                                          GLsizei height, GLsizei depth, GLint border, const void* data,
                                          size_t size) {
    if (boundPixelUnpackBuffer_ != 0) {
        setSyntheticError(GL_INVALID_OPERATION);
        return;
    }
    compressedImage(target, level, internalformat, width, height, depth, border, data, size, true);
}

void WebGLVkContext::compressedTexSubImage3D(GLenum target, GLint level, GLint xoffset, GLint yoffset,
                                             GLint zoffset, GLsizei width, GLsizei height, GLsizei depth,
                                             GLenum format, const void* data, size_t size) {
    if (boundPixelUnpackBuffer_ != 0) {
        setSyntheticError(GL_INVALID_OPERATION);
        return;
    }
    compressedSubImage(target, level, xoffset, yoffset, zoffset, width, height, depth, format, data, size, true);
}

void WebGLVkContext::compressedTexImageFromPBO(GLenum target, GLint level, GLenum internalformat, GLsizei width,
                                               GLsizei height, GLsizei depth, GLint border, GLsizei size,
                                               GLintptr offset, bool is3D) {
    PixelSource src;
    if (!pboSource(offset, src)) return;
    if (size < 0 || static_cast<size_t>(size) > src.size) {
        setSyntheticError(GL_INVALID_OPERATION);
        return;
    }
    compressedImage(target, level, internalformat, width, height, depth, border, src.data,
                    static_cast<size_t>(size), is3D);
}

void WebGLVkContext::compressedTexSubImageFromPBO(GLenum target, GLint level, GLint xoffset, GLint yoffset,
                                                  GLint zoffset, GLsizei width, GLsizei height, GLsizei depth,
                                                  GLenum format, GLsizei size, GLintptr offset, bool is3D) {
    PixelSource src;
    if (!pboSource(offset, src)) return;
    if (size < 0 || static_cast<size_t>(size) > src.size) {
        setSyntheticError(GL_INVALID_OPERATION);
        return;
    }
    compressedSubImage(target, level, xoffset, yoffset, zoffset, width, height, depth, format, src.data,
                       static_cast<size_t>(size), is3D);
}

// ---------------------------------------------------------------------------
// copyTex*
// ---------------------------------------------------------------------------

// Copy the read buffer's (x, y, width, height) into `level` of `tex` at
// (xoffset, yoffset) of layer (or 3D slice) `layer`. ES 3.0 3.8.5: the
// texture's format must take the buffer's components, kind and encoding.
// Pixels outside the read framebuffer are left alone.
void WebGLVkContext::copyFromReadBuffer(VkTextureResource& tex, uint32_t level, uint32_t layer, int32_t xoffset,
                                        int32_t yoffset, GLint x, GLint y, GLsizei width, GLsizei height) {
    Surface src;
    if (!readColorSurface(src)) return;
    const TexFormat& tf = tex.levels[level].format;
    if (!src || src.samples > VK_SAMPLE_COUNT_1_BIT || tf.compressed || tf.isDepthOrStencil()) {
        setSyntheticError(GL_INVALID_OPERATION);
        return;
    }
    const FormatBits bits = formatBits(src.format);
    const bool needsAlpha = tf.baseFormat == GL_RGBA || tf.baseFormat == GL_RGBA_INTEGER ||
                            tf.baseFormat == GL_ALPHA || tf.baseFormat == GL_LUMINANCE_ALPHA;
    const bool needsBlue = tf.baseFormat == GL_RGB || tf.baseFormat == GL_RGBA || tf.baseFormat == GL_RGB_INTEGER ||
                           tf.baseFormat == GL_RGBA_INTEGER;
    const bool needsGreen = needsBlue || tf.baseFormat == GL_RG || tf.baseFormat == GL_RG_INTEGER;
    if (isIntegerFormat(src.format) != tf.isInteger() ||
        (tf.isInteger() && isSignedIntegerFormat(src.format) != (tf.kind == TexKind::Int)) ||
        isFixedPoint(src.format) != isFixedPoint(tf.format) || isSrgb(src.format) != isSrgb(tf.format) ||
        (needsAlpha && (bits.alpha == 0 || src.alphaOne)) || (needsBlue && bits.blue == 0) ||
        (needsGreen && bits.green == 0)) {
        setSyntheticError(GL_INVALID_OPERATION);
        return;
    }
    // Reading the level being written is a feedback loop (WebGL 2 5.18).
    const uint32_t srcLayer = tex.is3D() ? static_cast<uint32_t>(src.z) : src.layer;
    if (src.tex == &tex && src.level == level && srcLayer == layer) {
        setSyntheticError(GL_INVALID_OPERATION);
        return;
    }

    // The part of the rectangle inside the read framebuffer.
    const int32_t x0 = std::max(x, 0), y0 = std::max(y, 0);
    const int32_t x1 = static_cast<int32_t>(std::min<int64_t>(int64_t{x} + width, src.width));
    const int32_t y1 = static_cast<int32_t>(std::min<int64_t>(int64_t{y} + height, src.height));
    if (x1 <= x0 || y1 <= y0) return;
    const uint32_t w = static_cast<uint32_t>(x1 - x0), h = static_cast<uint32_t>(y1 - y0);
    const int32_t dx = xoffset + (x0 - x), dy = yoffset + (y0 - y);

    // ALPHA / LUMINANCE_ALPHA keep alpha where no copy can put it (in R or
    // G): through the CPU, as readPixels then texSubImage would.
    const bool swizzled = tf.baseFormat == GL_ALPHA || tf.baseFormat == GL_LUMINANCE_ALPHA;
    const VkPhysicalDevice device = context_.physicalDevice();
    auto has = [&](VkFormat f, VkFormatFeatureFlags features) {
        VkFormatProperties props;
        vkGetPhysicalDeviceFormatProperties(device, f, &props);
        return (props.optimalTilingFeatures & features) == features;
    };
    const bool copy = src.format == tex.format && !swizzled;
    const bool blit = !copy && !swizzled && has(src.format, VK_FORMAT_FEATURE_BLIT_SRC_BIT) &&
                      has(tex.format, VK_FORMAT_FEATURE_BLIT_DST_BIT);
    if (!copy && !blit) {
        std::vector<uint8_t> rgba(static_cast<size_t>(w) * h * 4);
        const PackState saved = pack_;
        pack_ = PackState{};
        pack_.alignment = 1;
        readPixelsInto(x0, y0, static_cast<GLsizei>(w), static_cast<GLsizei>(h), GL_RGBA, GL_UNSIGNED_BYTE,
                       rgba.data());
        pack_ = saved;
        PixelSource pixels{rgba.data(), rgba.size(), GL_RGBA, GL_UNSIGNED_BYTE};
        pixels.raw = true;
        writeTexels(tex, level, dx, dy, static_cast<int32_t>(layer), w, h, 1, pixels);
        return;
    }

    VkCommandBuffer cmd = transferCommands();
    const VkImageLayout restore = surfaceLayout(src);
    transitionSurface(cmd, src, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    const uint32_t dstLayer = tex.is3D() ? 0 : layer;
    const int32_t dz = tex.is3D() ? static_cast<int32_t>(layer) : 0;
    transitionTextureRange(cmd, tex, level, 1, dstLayer, 1, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    const VkImageSubresourceLayers srcLayers{VK_IMAGE_ASPECT_COLOR_BIT, src.level, src.layer, 1};
    const VkImageSubresourceLayers dstLayers{VK_IMAGE_ASPECT_COLOR_BIT, level, dstLayer, 1};
    // The canvas is stored top-down: GL row y0 is image row H-1-y0.
    const int32_t top = src.topDown() ? static_cast<int32_t>(src.height) - y1 : y0;
    if (copy) {
        std::vector<VkImageCopy> regions;
        for (uint32_t row = 0; row < (src.topDown() ? h : 1u); ++row) {
            VkImageCopy c{};
            c.srcSubresource = srcLayers;
            c.dstSubresource = dstLayers;
            c.srcOffset = {x0, top + static_cast<int32_t>(row), src.z};
            c.dstOffset = {dx, src.topDown() ? dy + static_cast<int32_t>(h - 1 - row) : dy, dz};
            c.extent = {w, src.topDown() ? 1u : h, 1};
            regions.push_back(c);
        }
        vkCmdCopyImage(cmd, src.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, tex.image,
                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, static_cast<uint32_t>(regions.size()), regions.data());
    } else {
        VkImageBlit b{};
        b.srcSubresource = srcLayers;
        b.srcOffsets[0] = {x0, src.topDown() ? top + static_cast<int32_t>(h) : top, src.z};
        b.srcOffsets[1] = {x1, src.topDown() ? top : top + static_cast<int32_t>(h), src.z + 1};
        b.dstSubresource = dstLayers;
        b.dstOffsets[0] = {dx, dy, dz};
        b.dstOffsets[1] = {dx + static_cast<int32_t>(w), dy + static_cast<int32_t>(h), dz + 1};
        vkCmdBlitImage(cmd, src.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, tex.image,
                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &b, VK_FILTER_NEAREST);
    }
    transitionTextureRange(cmd, tex, level, 1, dstLayer, 1, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    transitionSurface(cmd, src, src.source == Surface::Source::Texture ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
                                                                       : restore);
}

void WebGLVkContext::copyTexImage2D(GLenum target, GLint level, GLenum internalformat, GLint x, GLint y,
                                    GLsizei width, GLsizei height, GLint border) {
    VkTextureResource* tex = textureForTarget(target, k2DTargets);
    if (!tex || !validateLevelSize(target, level, width, height, 1)) return;
    if (border != 0) {
        setSyntheticError(GL_INVALID_VALUE);
        return;
    }
    Surface src;
    if (!readColorSurface(src)) return;
    // An unsized format takes the read buffer's 8-bit fixed-point texels.
    TexFormat tf;
    GLenum error = GL_NO_ERROR;
    const bool unsized = internalformat == GL_RGBA || internalformat == GL_RGB || internalformat == GL_ALPHA ||
                         internalformat == GL_LUMINANCE || internalformat == GL_LUMINANCE_ALPHA;
    if (unsized ? !resolveTexFormat(context_.physicalDevice(), static_cast<GLint>(internalformat), internalformat,
                                    GL_UNSIGNED_BYTE, tf, error)
                : !resolveSizedFormat(context_.physicalDevice(), internalformat, tf)) {
        setSyntheticError(GL_INVALID_ENUM);
        return;
    }
    if (tex->immutable || tf.isDepthOrStencil() || !src) {
        setSyntheticError(GL_INVALID_OPERATION);
        return;
    }
    const auto l = static_cast<uint32_t>(level), face = faceOf(target);
    if (width == 0 || height == 0) return;
    if (!defineLevel(*tex, l, face, tf, static_cast<uint32_t>(width), static_cast<uint32_t>(height), 1)) return;
    // WebGL: what lies outside the read framebuffer copies as zeros.
    zeroTexels(*tex, l, face, 1);
    copyFromReadBuffer(*tex, l, face, 0, 0, x, y, width, height);
}

void WebGLVkContext::copyTexSubImage2D(GLenum target, GLint level, GLint xoffset, GLint yoffset, GLint x, GLint y,
                                       GLsizei width, GLsizei height) {
    VkTextureResource* tex = textureForTarget(target, k2DTargets);
    if (!tex) return;
    if (level < 0 || xoffset < 0 || yoffset < 0 || width < 0 || height < 0) {
        setSyntheticError(GL_INVALID_VALUE);
        return;
    }
    const auto l = static_cast<uint32_t>(level), face = faceOf(target);
    if (l >= tex->levels.size() || !(tex->levels[l].faces & (1u << face))) {
        setSyntheticError(GL_INVALID_OPERATION);
        return;
    }
    const TexLevel& lv = tex->levels[l];
    if (static_cast<int64_t>(xoffset) + width > lv.width || static_cast<int64_t>(yoffset) + height > lv.height) {
        setSyntheticError(GL_INVALID_VALUE);
        return;
    }
    if (width == 0 || height == 0) return;
    copyFromReadBuffer(*tex, l, face, xoffset, yoffset, x, y, width, height);
}

void WebGLVkContext::copyTexSubImage3D(GLenum target, GLint level, GLint xoffset, GLint yoffset, GLint zoffset,
                                       GLint x, GLint y, GLsizei width, GLsizei height) {
    VkTextureResource* tex = textureForTarget(target, k3DTargets);
    if (!tex) return;
    if (level < 0 || xoffset < 0 || yoffset < 0 || zoffset < 0 || width < 0 || height < 0) {
        setSyntheticError(GL_INVALID_VALUE);
        return;
    }
    const auto l = static_cast<uint32_t>(level);
    if (l >= tex->levels.size() || tex->levels[l].faces == 0) {
        setSyntheticError(GL_INVALID_OPERATION);
        return;
    }
    const TexLevel& lv = tex->levels[l];
    if (static_cast<int64_t>(xoffset) + width > lv.width || static_cast<int64_t>(yoffset) + height > lv.height ||
        static_cast<uint32_t>(zoffset) >= lv.depth) {
        setSyntheticError(GL_INVALID_VALUE);
        return;
    }
    if (width == 0 || height == 0) return;
    copyFromReadBuffer(*tex, l, static_cast<uint32_t>(zoffset), xoffset, yoffset, x, y, width, height);
}

// ---------------------------------------------------------------------------
// generateMipmap
// ---------------------------------------------------------------------------

// Levels base+1 .. q from the base level, each a linear blit of the one
// above (ES 3.0 3.8.10), over every face, layer or slice. The base level
// must be defined, color-renderable and filterable.
void WebGLVkContext::generateMipmap(GLenum target) {
    if (isCubeFace(target)) {
        setSyntheticError(GL_INVALID_ENUM);
        return;
    }
    VkTextureResource* tex = textureForTarget(target, kTextureTargets);
    if (!tex) return;
    const bool cube = tex->target == GL_TEXTURE_CUBE_MAP;
    uint32_t base = static_cast<uint32_t>(std::max(tex->baseLevel, 0));
    uint32_t maxLevel = static_cast<uint32_t>(std::max(tex->maxLevel, 0));
    if (tex->immutable) {
        base = std::min(base, tex->immutableLevels - 1);
        maxLevel = std::clamp(maxLevel, base, tex->immutableLevels - 1);
    }
    if (base >= tex->levels.size() || tex->levels[base].faces != (cube ? 0x3F : 0x01) ||
        tex->levels[base].width == 0 || (cube && tex->levels[base].width != tex->levels[base].height)) {
        setSyntheticError(GL_INVALID_OPERATION);
        return;
    }
    const TexLevel b = tex->levels[base];
    const TexFormat tf = b.format;
    const bool renderable = !tf.sized || colorRenderableFormat(tf.internalformat) != VK_FORMAT_UNDEFINED;
    const VkFormatFeatureFlags needs = VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT |
                                       VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT;
    if (tf.compressed || tf.isDepthOrStencil() || tf.isInteger() || !renderable || !formatSupports(tf.format, needs)) {
        setSyntheticError(GL_INVALID_OPERATION);
        return;
    }
    const uint32_t extent = std::max({b.width, b.height, tex->is3D() ? b.depth : 1u});
    const uint32_t last = std::min(base + static_cast<uint32_t>(std::floor(std::log2(extent))), maxLevel);
    if (last == base) return;
    // Define the levels (reallocating the image if they do not fit it).
    for (uint32_t l = base + 1; l <= last; ++l) {
        const uint32_t shift = l - base;
        const uint32_t w = std::max(1u, b.width >> shift), h = std::max(1u, b.height >> shift);
        const uint32_t d = tex->is3D() ? std::max(1u, b.depth >> shift) : b.depth;
        for (uint32_t face = 0; face < (cube ? 6u : 1u); ++face)
            if (!defineLevel(*tex, l, face, tf, w, h, d)) return;
    }
    VkCommandBuffer cmd = transferCommands();
    const uint32_t layers = tex->arrayLayers;
    for (uint32_t l = base + 1; l <= last; ++l) {
        const TexLevel& from = tex->levels[l - 1];
        const TexLevel& to = tex->levels[l];
        transitionTextureRange(cmd, *tex, l - 1, 1, 0, layers, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        transitionTextureRange(cmd, *tex, l, 1, 0, layers, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        VkImageBlit blit{};
        blit.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, l - 1, 0, layers};
        blit.srcOffsets[1] = {static_cast<int32_t>(from.width), static_cast<int32_t>(from.height),
                              tex->is3D() ? static_cast<int32_t>(from.depth) : 1};
        blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, l, 0, layers};
        blit.dstOffsets[1] = {static_cast<int32_t>(to.width), static_cast<int32_t>(to.height),
                              tex->is3D() ? static_cast<int32_t>(to.depth) : 1};
        vkCmdBlitImage(cmd, tex->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, tex->image,
                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_LINEAR);
    }
    transitionTextureRange(cmd, *tex, base, last - base + 1, 0, layers, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
}

} // namespace bro::webgl::vk
