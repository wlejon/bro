#include "render/pixel_convert.h"

#include <include/core/SkPixmap.h>

#include <cstring>

namespace bro::render {

void copyPixels32(void* dst, size_t dstStride, const void* src, size_t srcStride,
                  uint32_t width, uint32_t height, bool swapRB) {
    const size_t rowBytes = static_cast<size_t>(width) * 4;
    auto* d = static_cast<uint8_t*>(dst);
    const auto* s = static_cast<const uint8_t*>(src);
    if (!swapRB) {
        if (dstStride == rowBytes && srcStride == rowBytes) {
            std::memcpy(d, s, rowBytes * height);
            return;
        }
        for (uint32_t y = 0; y < height; ++y)
            std::memcpy(d + y * dstStride, s + y * srcStride, rowBytes);
        return;
    }
    for (uint32_t y = 0; y < height; ++y) {
        const uint8_t* rs = s + y * srcStride;
        uint8_t* rd = d + y * dstStride;
        for (uint32_t x = 0; x < width; ++x) {
            rd[0] = rs[2];
            rd[1] = rs[1];
            rd[2] = rs[0];
            rd[3] = rs[3];
            rs += 4;
            rd += 4;
        }
    }
}

std::vector<uint8_t> pixmapToRgba(const SkPixmap& pixmap) {
    const bool bgra = pixmap.colorType() == kBGRA_8888_SkColorType;
    if (!bgra && pixmap.colorType() != kRGBA_8888_SkColorType) return {};
    const auto w = static_cast<uint32_t>(pixmap.width());
    const auto h = static_cast<uint32_t>(pixmap.height());
    std::vector<uint8_t> rgba(static_cast<size_t>(w) * h * 4);
    copyPixels32(rgba.data(), static_cast<size_t>(w) * 4, pixmap.addr(), pixmap.rowBytes(),
                 w, h, bgra);
    return rgba;
}

} // namespace bro::render
