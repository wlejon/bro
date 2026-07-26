#include "video/yuv_to_rgb.h"

#include <algorithm>
#include <cstring>

namespace bro::video {

namespace {

// Saturation by table lookup rather than two compares per component.
//
// The BT.601 fixed-point matrix below can produce anything in [-277, 534]
// after the >>8, so a 1024-entry table offset by 384 covers the range with
// room to spare. This matters because the conversion runs on every displayed
// frame: at 720p it is ~2.8 million lookups, and the branchy std::clamp
// version was measured at 3.3 ms a frame — a fifth of a 60 Hz budget, and
// thirty times what decoding the frame cost.
struct ClampTable {
    uint8_t v[1024];
    constexpr ClampTable() : v{} {
        for (int i = 0; i < 1024; ++i) {
            const int x = i - 384;
            v[i] = static_cast<uint8_t>(x < 0 ? 0 : (x > 255 ? 255 : x));
        }
    }
};
constexpr ClampTable kClamp{};

inline uint8_t sat(int v) { return kClamp.v[v + 384]; }

// Measured against a branchless `v < 0 ? 0 : (v > 255 ? 255 : v)`: the table
// wins by ~20%. MSVC does not auto-vectorize either form (the interleaved
// RGBA store defeats it), so the compares are pure added work. Getting
// meaningfully faster than this needs hand-written SIMD, or — better — not
// doing the conversion on the CPU at all.

} // namespace

void i420ToRgba(const uint8_t* y, const uint8_t* u, const uint8_t* v,
                int strideY, int strideU, int strideV,
                int width, int height,
                uint8_t* dst, int dstStride) {
    if (dstStride <= 0) dstStride = width * 4;

    for (int row = 0; row < height; ++row) {
        const uint8_t* yRow = y + static_cast<size_t>(row) * strideY;
        const uint8_t* uRow = u + static_cast<size_t>(row / 2) * strideU;
        const uint8_t* vRow = v + static_cast<size_t>(row / 2) * strideV;
        uint8_t* out = dst + static_cast<size_t>(row) * dstStride;

        // Two luma samples share one chroma sample in 4:2:0, so the chroma
        // load and its three multiplies are done once per pair instead of
        // once per pixel.
        int col = 0;
        for (; col + 1 < width; col += 2) {
            const int ui = uRow[col >> 1] - 128;
            const int vi = vRow[col >> 1] - 128;
            const int rc =  409 * vi + 128;
            const int gc = -100 * ui - 208 * vi + 128;
            const int bc =  516 * ui + 128;

            const int c0 = 298 * (yRow[col] - 16);
            out[0] = sat((c0 + rc) >> 8);
            out[1] = sat((c0 + gc) >> 8);
            out[2] = sat((c0 + bc) >> 8);
            out[3] = 255;

            const int c1 = 298 * (yRow[col + 1] - 16);
            out[4] = sat((c1 + rc) >> 8);
            out[5] = sat((c1 + gc) >> 8);
            out[6] = sat((c1 + bc) >> 8);
            out[7] = 255;

            out += 8;
        }
        if (col < width) {   // odd width: last column alone
            const int ui = uRow[col >> 1] - 128;
            const int vi = vRow[col >> 1] - 128;
            const int c = 298 * (yRow[col] - 16);
            out[0] = sat((c + 409 * vi + 128) >> 8);
            out[1] = sat((c - 100 * ui - 208 * vi + 128) >> 8);
            out[2] = sat((c + 516 * ui + 128) >> 8);
            out[3] = 255;
        }
    }
}

void i420ToRgbaScaled(const uint8_t* y, const uint8_t* u, const uint8_t* v,
                      int strideY, int strideU, int strideV,
                      int srcW, int srcH,
                      uint8_t* dst, int dstStride, int dstW, int dstH) {
    if (srcW <= 0 || srcH <= 0 || dstW <= 0 || dstH <= 0) return;
    if (dstStride <= 0) dstStride = dstW * 4;

    const int cw = (srcW + 1) / 2, chh = (srcH + 1) / 2;

    for (int dy = 0; dy < dstH; ++dy) {
        int y0 = static_cast<int>(static_cast<int64_t>(dy) * srcH / dstH);
        int y1 = static_cast<int>(static_cast<int64_t>(dy + 1) * srcH / dstH);
        if (y1 <= y0) y1 = y0 + 1;
        if (y1 > srcH) y1 = srcH;

        uint8_t* out = dst + static_cast<size_t>(dy) * dstStride;
        for (int dx = 0; dx < dstW; ++dx) {
            int x0 = static_cast<int>(static_cast<int64_t>(dx) * srcW / dstW);
            int x1 = static_cast<int>(static_cast<int64_t>(dx + 1) * srcW / dstW);
            if (x1 <= x0) x1 = x0 + 1;
            if (x1 > srcW) x1 = srcW;

            int ySum = 0, yN = 0;
            for (int sy = y0; sy < y1; ++sy) {
                const uint8_t* row = y + static_cast<size_t>(sy) * strideY;
                for (int sx = x0; sx < x1; ++sx) ySum += row[sx];
                yN += x1 - x0;
            }

            // Chroma is half resolution in both axes, so the same source box
            // maps to half the coordinates — and never to an empty range.
            int cy0 = y0 / 2, cy1 = (y1 + 1) / 2;
            int cx0 = x0 / 2, cx1 = (x1 + 1) / 2;
            if (cy1 <= cy0) cy1 = cy0 + 1;
            if (cx1 <= cx0) cx1 = cx0 + 1;
            if (cy1 > chh) cy1 = chh;
            if (cx1 > cw) cx1 = cw;

            int uSum = 0, vSum = 0, cN = 0;
            for (int sy = cy0; sy < cy1; ++sy) {
                const uint8_t* uRow = u + static_cast<size_t>(sy) * strideU;
                const uint8_t* vRow = v + static_cast<size_t>(sy) * strideV;
                for (int sx = cx0; sx < cx1; ++sx) { uSum += uRow[sx]; vSum += vRow[sx]; }
                cN += cx1 - cx0;
            }

            const int yy = yN ? ySum / yN : 16;
            const int ui = (cN ? uSum / cN : 128) - 128;
            const int vi = (cN ? vSum / cN : 128) - 128;
            const int c = 298 * (yy - 16);
            out[0] = sat((c + 409 * vi + 128) >> 8);
            out[1] = sat((c - 100 * ui - 208 * vi + 128) >> 8);
            out[2] = sat((c + 516 * ui + 128) >> 8);
            out[3] = 255;
            out += 4;
        }
    }
}

} // namespace bro::video
