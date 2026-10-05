// readPixels: the read framebuffer's read buffer, copied region-only into
// readback memory, then converted to the format/type WebGL 2 lets that
// buffer be read as, honouring PACK_ALIGNMENT. Pixels outside the
// framebuffer leave the destination untouched.

#include "webgl/vulkan/webgl_vk_context.h"
#include "webgl/vulkan/webgl_vk_formats.h"
#include "render/vulkan_util.h"
#include "util/log.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace bro::webgl::vk {

namespace {

// What a read buffer may be read as (WebGL 2 / ES 3.0 4.3.2): normalized
// buffers as RGBA/UNSIGNED_BYTE, float ones as RGBA/FLOAT, integer ones as
// RGBA_INTEGER/INT or UNSIGNED_INT. 0 bytes per pixel for anything else.
uint32_t readPixelBytes(VkFormat format, GLenum glFormat, GLenum type) {
    if (isIntegerFormat(format)) {
        if (glFormat != GL_RGBA_INTEGER) return 0;
        const GLenum want = isSignedIntegerFormat(format) ? GL_INT : GL_UNSIGNED_INT;
        return type == want ? 16 : 0;
    }
    if (glFormat != GL_RGBA) return 0;
    const FormatBits bits = formatBits(format);
    const bool normalized = bits.red == 8 || format == VK_FORMAT_A2B10G10R10_UNORM_PACK32;
    if (type == GL_UNSIGNED_BYTE) return normalized ? 4 : 0;
    if (type == GL_FLOAT) return normalized ? 0 : 16;
    return 0;
}

uint8_t toUnorm8(float v) {
    return static_cast<uint8_t>(std::lround(std::clamp(v, 0.0f, 1.0f) * 255.0f));
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
    const uint32_t pixelBytes = readPixelBytes(src.format, format, type);
    if (pixelBytes == 0) {
        setSyntheticError(GL_INVALID_OPERATION);
        return 0;
    }
    if (width == 0 || height == 0) return 0;
    const size_t rowBytes = static_cast<size_t>(width) * pixelBytes;
    const size_t stride = (rowBytes + packAlignment_ - 1) / packAlignment_ * packAlignment_;
    return stride * (static_cast<size_t>(height) - 1) + rowBytes;
}

void WebGLVkContext::readPixels(GLint x, GLint y, GLsizei width, GLsizei height,
                                GLenum format, GLenum type, void* pixels) {
    if (!pixels) return;
    if (readPixelsByteCount(width, height, format, type) == 0) return;
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
    region.imageOffset = {x0, top, 0};
    region.imageExtent = {w, h, 1};
    vkCmdCopyImageToBuffer(cmd, src.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, readback_.buffer, 1, &region);
    render::cmdMemoryBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
                             VK_PIPELINE_STAGE_HOST_BIT, VK_ACCESS_HOST_READ_BIT);
    transitionSurface(cmd, src, src.source == Surface::Source::Texture ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
                                                                       : restore);
    if (!waitForCommands()) return;

    const uint32_t pixelBytes = readPixelBytes(src.format, format, type);
    const size_t rowBytes = static_cast<size_t>(width) * pixelBytes;
    const size_t stride = (rowBytes + packAlignment_ - 1) / packAlignment_ * packAlignment_;
    const auto* texels = static_cast<const uint8_t*>(mapped);
    const bool integer = isIntegerFormat(src.format);
    const bool isSigned = isSignedIntegerFormat(src.format);
    for (uint32_t row = 0; row < h; ++row) {
        // GL row y0 + row of the framebuffer; its place in the copy.
        const uint32_t copyRow = src.topDown() ? h - 1 - row : row;
        const uint8_t* in = texels + static_cast<size_t>(copyRow) * w * texelSize;
        uint8_t* out = static_cast<uint8_t*>(pixels) + static_cast<size_t>(y0 + static_cast<int32_t>(row) - y) * stride +
                       static_cast<size_t>(x0 - x) * pixelBytes;
        for (uint32_t col = 0; col < w; ++col, in += texelSize, out += pixelBytes) {
            const Texel t = decodeTexel(src.format, in);
            if (integer) {
                if (isSigned) std::memcpy(out, t.i, 16);
                else std::memcpy(out, t.u, 16);
            } else if (type == GL_FLOAT) {
                std::memcpy(out, t.f, 16);
            } else {
                for (int c = 0; c < 4; ++c) out[c] = toUnorm8(t.f[c]);
            }
        }
    }
}

void WebGLVkContext::readPixelsToPBO(GLint x, GLint y, GLsizei width, GLsizei height,
                                     GLenum format, GLenum type, GLintptr offset) {
    auto it = boundPixelPackBuffer_ != 0 ? buffers_.find(boundPixelPackBuffer_) : buffers_.end();
    if (it == buffers_.end()) {
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
    readPixels(x, y, width, height, format, type, pbo.shadowData.data() + offset);
    uploadToBuffer(pbo, static_cast<VkDeviceSize>(offset), pbo.shadowData.data() + offset, byteCount);
}

} // namespace bro::webgl::vk
