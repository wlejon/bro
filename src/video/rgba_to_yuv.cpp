#include "video/rgba_to_yuv.h"

#include <algorithm>

namespace bro::video {

namespace {
inline uint8_t clip(int v) {
    return static_cast<uint8_t>(std::clamp(v, 0, 255));
}
}

void rgbaToI420(const uint8_t* rgba, int srcStride,
                int width, int height,
                uint8_t* y, int strideY,
                uint8_t* u, int strideU,
                uint8_t* v, int strideV) {
    if (srcStride <= 0) srcStride = width * 4;
    if (strideY <= 0) strideY = width;
    if (strideU <= 0) strideU = width / 2;
    if (strideV <= 0) strideV = width / 2;

    // Y plane: per-pixel BT.601 limited-range luma.
    // Coefficients * 256 to keep arithmetic in int.
    for (int row = 0; row < height; ++row) {
        const uint8_t* src = rgba + row * srcStride;
        uint8_t* dst = y + row * strideY;
        for (int col = 0; col < width; ++col) {
            const int r = src[0], g = src[1], b = src[2];
            // Y = 0.257 R + 0.504 G + 0.098 B + 16
            const int yi = (66 * r + 129 * g + 25 * b + 128) >> 8;
            dst[col] = clip(yi + 16);
            src += 4;
        }
    }

    // U / V planes: box-average each 2x2 RGB block, then apply chroma matrix.
    // Odd trailing row/col is just dropped — width/height are required even
    // by the contract.
    const int chromaW = width / 2;
    const int chromaH = height / 2;
    for (int row = 0; row < chromaH; ++row) {
        const uint8_t* r0 = rgba + (row * 2) * srcStride;
        const uint8_t* r1 = rgba + (row * 2 + 1) * srcStride;
        uint8_t* uOut = u + row * strideU;
        uint8_t* vOut = v + row * strideV;
        for (int col = 0; col < chromaW; ++col) {
            const int idx0 = col * 8;     // 2 pixels * 4 bytes
            const int idx1 = idx0 + 4;
            const int rAvg = (r0[idx0] + r0[idx1] + r1[idx0] + r1[idx1] + 2) >> 2;
            const int gAvg = (r0[idx0+1] + r0[idx1+1] + r1[idx0+1] + r1[idx1+1] + 2) >> 2;
            const int bAvg = (r0[idx0+2] + r0[idx1+2] + r1[idx0+2] + r1[idx1+2] + 2) >> 2;
            // U = -0.148 R - 0.291 G + 0.439 B + 128
            // V =  0.439 R - 0.368 G - 0.071 B + 128
            const int ui = (-38 * rAvg - 74 * gAvg + 112 * bAvg + 128) >> 8;
            const int vi = (112 * rAvg - 94 * gAvg - 18 * bAvg + 128) >> 8;
            uOut[col] = clip(ui + 128);
            vOut[col] = clip(vi + 128);
        }
    }
}

} // namespace bro::video
