#pragma once

#include "webgl/webgl_types.h"

#include <vulkan/vulkan.h>
#include <array>
#include <cstddef>
#include <cstdint>

namespace bro::webgl::vk {

/// What a texture's texels are, as sampling and attachment see them.
enum class TexKind : uint8_t { Float, Int, Uint, Depth, DepthStencil, Stencil };

/// A GL texture internal format as this device stores it.
///
/// The Vulkan format is always one the device can sample: formats Vulkan
/// devices rarely store (three-component RGB, the 16-bit packed formats) are
/// widened to RGBA, with `alphaOne` set when GL's format has no alpha (the
/// stored alpha is kept at one and never written). The WebGL 1 formats
/// LUMINANCE / ALPHA / LUMINANCE_ALPHA are one- or two-channel images read
/// through a swizzle.
struct TexFormat {
    GLenum internalformat = 0;  // sized, as GL reports it
    GLenum baseFormat = 0;      // RGBA, RGB, RG, RED, LUMINANCE, ..., DEPTH_COMPONENT, DEPTH_STENCIL
    VkFormat format = VK_FORMAT_UNDEFINED;
    TexKind kind = TexKind::Float;
    VkComponentMapping swizzle{VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY,
                               VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY};
    // Stored channel i takes component channels[i] of a decoded texel (R, G,
    // B, A = 0..3), or the constant of `alphaOne` when -1.
    std::array<int8_t, 4> channels{0, 1, 2, 3};
    bool alphaOne = false;
    bool sized = true;          // false for the unsized (WebGL 1) internal formats
    // Compressed formats: block footprint and bytes per block.
    bool compressed = false;
    uint8_t blockWidth = 1, blockHeight = 1, blockBytes = 0;

    bool isDepthOrStencil() const { return kind == TexKind::Depth || kind == TexKind::DepthStencil ||
                                           kind == TexKind::Stencil; }
    bool isInteger() const { return kind == TexKind::Int || kind == TexKind::Uint; }
};

/// The format texImage* with (internalformat, format, type) creates, per the
/// ES 3.0 tables (sized formats: table 3.2; unsized: table 3.3), or false
/// with the GL error to raise.
bool resolveTexFormat(VkPhysicalDevice device, GLint internalformat, GLenum format, GLenum type,
                      TexFormat& out, GLenum& error);

/// The format texStorage* / a sized internalformat names, or false.
bool resolveSizedFormat(VkPhysicalDevice device, GLenum internalformat, TexFormat& out);

/// Whether client data of (format, type) may be written to a texture of
/// `tf` (texSubImage*): the combination resolves to the same internal format.
bool clientFormatCompatible(VkPhysicalDevice device, const TexFormat& tf, GLenum format, GLenum type);

/// A compressed internal format, whatever the device supports; false for
/// anything that is not one.
bool resolveCompressedFormat(GLenum internalformat, TexFormat& out);

/// Bytes a compressed image of width x height x depth occupies.
size_t compressedImageSize(const TexFormat& tf, uint32_t width, uint32_t height, uint32_t depth);

/// The compressed-texture extensions this device can back natively, each
/// with its formats (for COMPRESSED_TEXTURE_FORMATS and getExtension).
struct CompressedExtension {
    const char* name;
    const GLenum* formats;
    size_t count;
};
size_t compressedExtensions(const CompressedExtension*& out);
bool compressedExtensionSupported(VkPhysicalDevice device, const VkPhysicalDeviceFeatures& features,
                                  const CompressedExtension& ext);

/// Bytes of one client pixel of (format, type), or 0 for a combination that
/// is not a pixel transfer format.
uint32_t clientPixelSize(GLenum format, GLenum type);

/// Bytes one texel of the storage format occupies in a buffer copy of its
/// color aspect, its depth aspect, or (1) its stencil aspect.
uint32_t storageTexelSize(VkFormat format, VkImageAspectFlags aspect);

/// The internal format a framebuffer attachment of `format` reports when no
/// texture format says otherwise (renderbuffers, the canvas).
GLenum defaultInternalFormat(VkFormat format);

} // namespace bro::webgl::vk
