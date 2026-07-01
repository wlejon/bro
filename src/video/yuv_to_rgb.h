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

} // namespace bro::video
