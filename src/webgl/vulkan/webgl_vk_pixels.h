#pragma once

#include "webgl/vulkan/webgl_vk_texformat.h"
#include "webgl/webgl_types.h"

#include <cstddef>
#include <cstdint>

namespace bro::webgl::vk {

/// Pixel transfers between client memory and texture storage: GL's unpack
/// and pack rules (alignment, row length, image height, skips, WebGL's flip
/// and premultiply), and the conversion of every ES 3.0 client format/type
/// to and from the format a texture is stored in.

struct UnpackState {
    int32_t alignment = 4;
    int32_t rowLength = 0;    // 0: the image's width
    int32_t imageHeight = 0;  // 0: the image's height
    int32_t skipPixels = 0;
    int32_t skipRows = 0;
    int32_t skipImages = 0;
    bool flipY = false;
    bool premultiplyAlpha = false;
};

struct PackState {
    int32_t alignment = 4;
    int32_t rowLength = 0;
    int32_t skipPixels = 0;
    int32_t skipRows = 0;
};

/// Bytes client memory must hold for a width x height x depth image of
/// (format, type) under `state`: up to the end of the last pixel read.
size_t unpackedSize(const UnpackState& state, GLenum format, GLenum type, uint32_t width, uint32_t height,
                    uint32_t depth);

/// Convert a client image into tightly packed texels of `tf`'s storage,
/// image after image, row 0 first. Depth/stencil formats write the depth
/// aspect to `dst` and the stencil aspect (one byte a texel) to `stencil`.
void unpackPixels(const UnpackState& state, const void* src, GLenum format, GLenum type, uint32_t width,
                  uint32_t height, uint32_t depth, const TexFormat& tf, uint8_t* dst, uint8_t* stencil);

/// Bytes readPixels writes for a width x height rectangle of (format, type)
/// under `state`, counting from the start of client memory.
size_t packedSize(const PackState& state, GLenum format, GLenum type, uint32_t width, uint32_t height);

/// Byte offset and row stride of pixel (0, 0) of a packed rectangle.
struct PackLayout {
    size_t offset = 0;
    size_t rowStride = 0;
    uint32_t pixelSize = 0;
};
PackLayout packLayout(const PackState& state, GLenum format, GLenum type, uint32_t width);

/// Encode one texel of `storage` (colorTexelSize bytes at `src`) as client
/// (format, type) at `dst`; `alphaOne` reads its alpha as one.
void packTexel(VkFormat storage, const uint8_t* src, GLenum format, GLenum type, uint8_t* dst,
               bool alphaOne = false);

/// IEEE half <-> float.
uint16_t floatToHalf(float f);

} // namespace bro::webgl::vk
