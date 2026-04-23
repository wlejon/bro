#pragma once

#include <cstdint>

namespace bro::video {

// Convert an I420 (YUV 4:2:0 planar) frame to tightly-packed RGBA8.
// `dst` must have capacity for width*height*4 bytes. Uses BT.601 limited
// range — the default for VP8/VP9 when no color metadata is present.
//
// Temporary CPU path. A GPU shader that samples the three planes directly
// is the long-term home; doing it here first keeps the surface area tight
// and gives us something to diff against once the shader lands.
void i420ToRgba(const uint8_t* y, const uint8_t* u, const uint8_t* v,
                int strideY, int strideU, int strideV,
                int width, int height,
                uint8_t* dst, int dstStride);

} // namespace bro::video
