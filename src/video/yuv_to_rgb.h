#pragma once

#include <cstdint>

namespace bro::video {

// Convert an I420 (YUV 4:2:0 planar) frame to tightly-packed RGBA8.
// `dst` must have capacity for width*height*4 bytes. Uses BT.601 limited
// range — the default for VP8/VP9 when no color metadata is present.
//
// CPU conversion path; a GPU shader sampling the three planes directly
// would be faster for large frames but this keeps the surface area small
// and gives a reference to validate any future shader against.
void i420ToRgba(const uint8_t* y, const uint8_t* u, const uint8_t* v,
                int strideY, int strideU, int strideV,
                int width, int height,
                uint8_t* dst, int dstStride);

// Same conversion, box-filtered down to dstW x dstH on the way out. For
// thumbnails: converting a 1440p frame in full and then shrinking it costs
// 5 ms and 15 MB per thumbnail, while averaging straight out of the planes
// touches each source pixel once and writes only what is kept.
//
// Downscale only — enlarging works but is a blocky nearest-neighbour result,
// which is not what this is for.
void i420ToRgbaScaled(const uint8_t* y, const uint8_t* u, const uint8_t* v,
                      int strideY, int strideU, int strideV,
                      int srcW, int srcH,
                      uint8_t* dst, int dstStride, int dstW, int dstH);

} // namespace bro::video
