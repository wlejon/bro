#pragma once

#include "webgl/webgl_types.h"

#include <vulkan/vulkan.h>
#include <cstdint>

namespace bro::webgl::vk {

/// The Vulkan format a GL depth and/or stencil internal format is stored in
/// on this device, or VK_FORMAT_UNDEFINED for anything else. Every depth
/// image — the canvas's (DEPTH24_STENCIL8, as GL's default framebuffer),
/// renderbuffers, depth textures — goes through here, so equal GL formats
/// are equal Vulkan formats, which a depth blit between them requires.
VkFormat depthStencilFormat(VkPhysicalDevice device, GLenum internalformat);

/// The Vulkan format of a color-renderable sized internal format (a
/// renderbuffer's), or VK_FORMAT_UNDEFINED.
VkFormat colorRenderableFormat(GLenum internalformat);

/// Bits per channel, as getParameter(RED_BITS ... STENCIL_BITS) reports them.
struct FormatBits {
    int red = 0, green = 0, blue = 0, alpha = 0, depth = 0, stencil = 0;
};
FormatBits formatBits(VkFormat format);

/// Integer (non-normalized) color format: cleared with clearBufferiv/uiv,
/// never filtered or blended.
bool isIntegerFormat(VkFormat format);
bool isSignedIntegerFormat(VkFormat format);

/// Bytes per texel of a color format, as a copy to a buffer packs it; 0
/// for depth/stencil and formats readPixels does not decode.
uint32_t colorTexelSize(VkFormat format);

/// A texel of a color format: floats for normalized and float formats,
/// the integer bits for integer formats (read through `i` / `u`).
struct Texel {
    float f[4]{0.0f, 0.0f, 0.0f, 1.0f};
    int32_t i[4]{0, 0, 0, 1};
    uint32_t u[4]{0, 0, 0, 1};
};
/// Decode one texel of `format` (colorTexelSize(format) bytes at `src`).
Texel decodeTexel(VkFormat format, const uint8_t* src);

} // namespace bro::webgl::vk
