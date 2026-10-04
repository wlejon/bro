#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

class SkPixmap;

namespace bro::render {

/// Copy a `width` x `height` block of 32-bit pixels between buffers with their
/// own row strides, swapping the R and B bytes when `swapRB` (RGBA <-> BGRA).
void copyPixels32(void* dst, size_t dstStride, const void* src, size_t srcStride,
                  uint32_t width, uint32_t height, bool swapRB);

/// A tightly packed RGBA8 copy of an 8-bit RGBA or BGRA pixmap (empty for any
/// other color type).
std::vector<uint8_t> pixmapToRgba(const SkPixmap& pixmap);

} // namespace bro::render
