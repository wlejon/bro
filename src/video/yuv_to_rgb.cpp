#include "video/yuv_to_rgb.h"

#include <algorithm>

namespace bro::video {

namespace {
inline uint8_t clip(int v) {
    return static_cast<uint8_t>(std::clamp(v, 0, 255));
}
}

void i420ToRgba(const uint8_t* y, const uint8_t* u, const uint8_t* v,
                int strideY, int strideU, int strideV,
                int width, int height,
                uint8_t* dst, int dstStride) {
    if (dstStride <= 0) dstStride = width * 4;

    for (int row = 0; row < height; ++row) {
        const uint8_t* yRow = y + row * strideY;
        const uint8_t* uRow = u + (row / 2) * strideU;
        const uint8_t* vRow = v + (row / 2) * strideV;
        uint8_t* out = dst + row * dstStride;

        for (int col = 0; col < width; ++col) {
            const int yi = yRow[col] - 16;
            const int ui = uRow[col / 2] - 128;
            const int vi = vRow[col / 2] - 128;

            // BT.601 limited range, fixed-point (scale 256)
            const int c  = 298 * yi;
            const int r = (c + 409 * vi + 128) >> 8;
            const int g = (c - 100 * ui - 208 * vi + 128) >> 8;
            const int b = (c + 516 * ui + 128) >> 8;

            out[0] = clip(r);
            out[1] = clip(g);
            out[2] = clip(b);
            out[3] = 255;
            out += 4;
        }
    }
}

} // namespace bro::video
