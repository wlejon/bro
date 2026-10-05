// readPixels: the read framebuffer's read buffer, copied region-only into
// readback memory, then converted to the format/type WebGL 2 lets that
// buffer be read as, laid out by the PACK_* state. Pixels outside the
// framebuffer leave the destination untouched. Into a pack buffer, a read of
// the buffer's own bytes is a GPU copy nothing waits for.

#include "webgl/vulkan/webgl_vk_context.h"
#include "webgl/vulkan/webgl_vk_formats.h"
#include "render/vulkan_util.h"
#include "util/log.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

namespace bro::webgl::vk {

// IMPLEMENTATION_COLOR_READ_FORMAT / _TYPE: the read buffer's own channels
// and component type, the one combination besides ES 3.0's fixed one
// (RGBA with UNSIGNED_BYTE, FLOAT, INT or UNSIGNED_INT by kind) readPixels
// accepts.
void WebGLVkContext::implementationReadFormat(const Surface& s, GLenum& format, GLenum& type) const {
    const FormatBits bits = formatBits(s.format);
    const bool integer = isIntegerFormat(s.format);
    const int channels = bits.alpha > 0 && !s.alphaOne ? 4 : bits.blue > 0 ? 3 : bits.green > 0 ? 2 : 1;
    static constexpr GLenum kNormalized[] = {GL_RED, GL_RG, GL_RGB, GL_RGBA};
    static constexpr GLenum kInteger[] = {GL_RED_INTEGER, GL_RG_INTEGER, GL_RGB_INTEGER, GL_RGBA_INTEGER};
    format = (integer ? kInteger : kNormalized)[channels - 1];
    if (integer) type = isSignedIntegerFormat(s.format) ? GL_INT : GL_UNSIGNED_INT;
    else if (s.format == VK_FORMAT_A2B10G10R10_UNORM_PACK32) type = GL_UNSIGNED_INT_2_10_10_10_REV;
    else if (bits.red == 8) type = GL_UNSIGNED_BYTE;
    else if (bits.red == 32) type = GL_FLOAT;
    else type = GL_HALF_FLOAT;
}

namespace {

// ES 3.0 4.3.2's fixed combination for a read buffer of `format`.
bool fixedReadFormat(VkFormat format, GLenum glFormat, GLenum type) {
    if (isIntegerFormat(format))
        return glFormat == GL_RGBA_INTEGER && type == (isSignedIntegerFormat(format) ? GL_INT : GL_UNSIGNED_INT);
    const FormatBits bits = formatBits(format);
    const bool normalized = bits.red == 8 || format == VK_FORMAT_A2B10G10R10_UNORM_PACK32;
    return glFormat == GL_RGBA && type == (normalized ? GL_UNSIGNED_BYTE : GL_FLOAT);
}

// Whether (format, type) is exactly the bytes `storage` holds, so a pack is a copy.
bool packIsIdentity(VkFormat storage, bool alphaOne, GLenum format, GLenum type) {
    if (alphaOne) return false;
    switch (storage) {
    case VK_FORMAT_R8G8B8A8_UNORM:
    case VK_FORMAT_R8G8B8A8_SRGB:        return format == GL_RGBA && type == GL_UNSIGNED_BYTE;
    case VK_FORMAT_R8G8_UNORM:           return format == GL_RG && type == GL_UNSIGNED_BYTE;
    case VK_FORMAT_R8_UNORM:             return format == GL_RED && type == GL_UNSIGNED_BYTE;
    case VK_FORMAT_R16G16B16A16_SFLOAT:  return format == GL_RGBA && type == GL_HALF_FLOAT;
    case VK_FORMAT_R16G16_SFLOAT:        return format == GL_RG && type == GL_HALF_FLOAT;
    case VK_FORMAT_R16_SFLOAT:           return format == GL_RED && type == GL_HALF_FLOAT;
    case VK_FORMAT_R32G32B32A32_SFLOAT:  return format == GL_RGBA && type == GL_FLOAT;
    case VK_FORMAT_R32G32_SFLOAT:        return format == GL_RG && type == GL_FLOAT;
    case VK_FORMAT_R32_SFLOAT:           return format == GL_RED && type == GL_FLOAT;
    case VK_FORMAT_R32G32B32A32_UINT:    return format == GL_RGBA_INTEGER && type == GL_UNSIGNED_INT;
    case VK_FORMAT_R32G32_UINT:          return format == GL_RG_INTEGER && type == GL_UNSIGNED_INT;
    case VK_FORMAT_R32_UINT:             return format == GL_RED_INTEGER && type == GL_UNSIGNED_INT;
    case VK_FORMAT_R32G32B32A32_SINT:    return format == GL_RGBA_INTEGER && type == GL_INT;
    case VK_FORMAT_R32G32_SINT:          return format == GL_RG_INTEGER && type == GL_INT;
    case VK_FORMAT_R32_SINT:             return format == GL_RED_INTEGER && type == GL_INT;
    case VK_FORMAT_A2B10G10R10_UNORM_PACK32:
        return format == GL_RGBA && type == GL_UNSIGNED_INT_2_10_10_10_REV;
    default: return false;
    }
}

} // namespace

size_t WebGLVkContext::readPixelsByteCount(GLsizei width, GLsizei height, GLenum format, GLenum type) {
    if (width < 0 || height < 0) {
        setSyntheticError(GL_INVALID_VALUE);
        return 0;
    }
    Surface src;
    if (!readColorSurface(src)) return 0;
    if (!src) {
        setSyntheticError(GL_INVALID_OPERATION);  // READ_BUFFER is NONE
        return 0;
    }
    GLenum implFormat = 0, implType = 0;
    implementationReadFormat(src, implFormat, implType);
    if (!fixedReadFormat(src.format, format, type) && (format != implFormat || type != implType)) {
        setSyntheticError(clientPixelSize(format, type) == 0 ? GL_INVALID_ENUM : GL_INVALID_OPERATION);
        return 0;
    }
    if (width == 0 || height == 0) return 0;
    return packedSize(pack_, format, type, static_cast<uint32_t>(width), static_cast<uint32_t>(height));
}

void WebGLVkContext::readPixels(GLint x, GLint y, GLsizei width, GLsizei height, GLenum format, GLenum type,
                                void* pixels, size_t size) {
    // WebGL 2: client memory is not a destination while a pack buffer is bound.
    if (boundPixelPackBuffer_ != 0) {
        setSyntheticError(GL_INVALID_OPERATION);
        return;
    }
    const size_t bytes = readPixelsByteCount(width, height, format, type);
    if (bytes == 0 || !pixels) return;
    if (size < bytes) {
        setSyntheticError(GL_INVALID_OPERATION);
        return;
    }
    readPixelsInto(x, y, width, height, format, type, pixels);
}

void WebGLVkContext::readPixelsInto(GLint x, GLint y, GLsizei width, GLsizei height, GLenum format, GLenum type,
                                    void* pixels) {
    Surface src;
    readColorSurface(src);
    if (src.samples > VK_SAMPLE_COUNT_1_BIT) {
        setSyntheticError(GL_INVALID_OPERATION);  // resolve with blitFramebuffer first
        return;
    }
    const uint32_t texelSize = colorTexelSize(src.format);
    if (texelSize == 0) {
        LOG_ERROR("WebGLVkContext: readPixels cannot decode format %d", static_cast<int>(src.format));
        setSyntheticError(GL_INVALID_OPERATION);
        return;
    }

    // The part of the rectangle inside the framebuffer, in GL coordinates.
    const int32_t x0 = std::max(x, 0), y0 = std::max(y, 0);
    const int32_t x1 = static_cast<int32_t>(std::min<int64_t>(int64_t{x} + width, src.width));
    const int32_t y1 = static_cast<int32_t>(std::min<int64_t>(int64_t{y} + height, src.height));
    if (x1 <= x0 || y1 <= y0) return;
    const uint32_t w = static_cast<uint32_t>(x1 - x0), h = static_cast<uint32_t>(y1 - y0);

    void* mapped = readbackMemory(static_cast<VkDeviceSize>(w) * h * texelSize);
    if (!mapped) return;
    VkCommandBuffer cmd = transferCommands();
    const VkImageLayout restore = surfaceLayout(src);
    transitionSurface(cmd, src, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    VkBufferImageCopy region{};
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, src.level, src.layer, 1};
    // The canvas is stored top-down: GL rows y0..y1 are image rows H-y1..H-y0.
    const int32_t top = src.topDown() ? static_cast<int32_t>(src.height) - y1 : y0;
    region.imageOffset = {x0, top, src.z};
    region.imageExtent = {w, h, 1};
    vkCmdCopyImageToBuffer(cmd, src.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, readback_.buffer, 1, &region);
    render::cmdMemoryBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
                             VK_PIPELINE_STAGE_HOST_BIT, VK_ACCESS_HOST_READ_BIT);
    transitionSurface(cmd, src, src.source == Surface::Source::Texture ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
                                                                       : restore);
    if (!waitForCommands()) return;

    const PackLayout layout = packLayout(pack_, format, type, static_cast<uint32_t>(width));
    const auto* texels = static_cast<const uint8_t*>(mapped);
    for (uint32_t row = 0; row < h; ++row) {
        // GL row y0 + row of the framebuffer; its place in the copy.
        const uint32_t copyRow = src.topDown() ? h - 1 - row : row;
        const uint8_t* in = texels + static_cast<size_t>(copyRow) * w * texelSize;
        uint8_t* out = static_cast<uint8_t*>(pixels) + layout.offset +
                       static_cast<size_t>(y0 + static_cast<int32_t>(row) - y) * layout.rowStride +
                       static_cast<size_t>(x0 - x) * layout.pixelSize;
        for (uint32_t col = 0; col < w; ++col, in += texelSize, out += layout.pixelSize)
            packTexel(src.format, in, format, type, out, src.alphaOne);
    }
}

void WebGLVkContext::readPixelsToPBO(GLint x, GLint y, GLsizei width, GLsizei height,
                                     GLenum format, GLenum type, GLintptr offset) {
    auto it = boundPixelPackBuffer_ != 0 ? buffers_.find(boundPixelPackBuffer_) : buffers_.end();
    if (it == buffers_.end() || it->second.isMapped) {
        setSyntheticError(GL_INVALID_OPERATION);
        return;
    }
    VkBufferResource& pbo = it->second;
    const size_t byteCount = readPixelsByteCount(width, height, format, type);
    if (byteCount == 0) return;
    if (offset < 0 || static_cast<size_t>(offset) + byteCount > pbo.shadowData.size()) {
        setSyntheticError(GL_INVALID_OPERATION);
        return;
    }
    if (readPixelsCopyToBuffer(x, y, width, height, format, type, pbo, offset)) return;
    // A conversion: read back, pack on the CPU, upload.
    syncShadow(pbo);
    readPixelsInto(x, y, width, height, format, type, pbo.shadowData.data() + offset);
    uploadToBuffer(pbo, static_cast<VkDeviceSize>(offset), pbo.shadowData.data() + offset, byteCount);
}

bool WebGLVkContext::readPixelsCopyToBuffer(GLint x, GLint y, GLsizei width, GLsizei height, GLenum format,
                                            GLenum type, VkBufferResource& pbo, GLintptr offset) {
    Surface src;
    readColorSurface(src);
    if (!src || src.samples > VK_SAMPLE_COUNT_1_BIT || !pbo.isValid() || !packIsIdentity(src.format, src.alphaOne, format, type))
        return false;
    const uint32_t texel = colorTexelSize(src.format);
    const PackLayout layout = packLayout(pack_, format, type, static_cast<uint32_t>(width));
    // Rows a whole number of texels apart, every row start 4-byte aligned:
    // what VkBufferImageCopy can express.
    if (texel == 0 || layout.rowStride % texel != 0 || layout.rowStride % 4 != 0) return false;

    const int32_t x0 = std::max(x, 0), y0 = std::max(y, 0);
    const int32_t x1 = static_cast<int32_t>(std::min<int64_t>(int64_t{x} + width, src.width));
    const int32_t y1 = static_cast<int32_t>(std::min<int64_t>(int64_t{y} + height, src.height));
    if (x1 <= x0 || y1 <= y0) return true;   // nothing inside the framebuffer: nothing written
    const uint32_t w = static_cast<uint32_t>(x1 - x0), h = static_cast<uint32_t>(y1 - y0);
    const VkDeviceSize base = static_cast<VkDeviceSize>(offset) + layout.offset +
                              static_cast<VkDeviceSize>(y0 - y) * layout.rowStride +
                              static_cast<VkDeviceSize>(x0 - x) * layout.pixelSize;
    if (base % 4 != 0 || base % texel != 0) return false;
    const VkDeviceSize span = static_cast<VkDeviceSize>(h - 1) * layout.rowStride + VkDeviceSize{w} * texel;

    // GL row y0 + r lands at base + r * rowStride. A top-down image (the
    // canvas) has those rows in reverse, so it copies a row per region.
    std::vector<VkBufferImageCopy> regions;
    VkBufferImageCopy region{};
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, src.level, src.layer, 1};
    region.bufferRowLength = static_cast<uint32_t>(layout.rowStride / texel);
    if (src.topDown()) {
        regions.reserve(h);
        for (uint32_t r = 0; r < h; ++r) {
            region.bufferOffset = base + VkDeviceSize{r} * layout.rowStride;
            region.imageOffset = {x0, static_cast<int32_t>(src.height) - y0 - 1 - static_cast<int32_t>(r), src.z};
            region.imageExtent = {w, 1, 1};
            regions.push_back(region);
        }
    } else {
        region.bufferOffset = base;
        region.imageOffset = {x0, y0, src.z};
        region.imageExtent = {w, h, 1};
        regions.push_back(region);
    }

    VkCommandBuffer cmd = transferCommands();
    const VkImageLayout restore = surfaceLayout(src);
    render::cmdBufferBarrier(cmd, pbo.buffer, base, span,
                             kBufferReadStages, VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_SHADER_WRITE_BIT,
                             VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
    transitionSurface(cmd, src, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    vkCmdCopyImageToBuffer(cmd, src.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, pbo.buffer,
                           static_cast<uint32_t>(regions.size()), regions.data());
    transitionSurface(cmd, src, src.source == Surface::Source::Texture ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
                                                                       : restore);
    render::cmdBufferBarrier(cmd, pbo.buffer, base, span, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_ACCESS_TRANSFER_WRITE_BIT, kBufferReadStages, kBufferReadAccess);
    // The GPU copy is now newer than bro's CPU one; whoever reads the CPU
    // copy (getBufferSubData, a mapping) brings it up to date first.
    pbo.deviceNewer = true;
    ++pbo.version;
    return true;
}

} // namespace bro::webgl::vk
