#pragma once

#include <cstdint>

namespace bro::video {

// Convert tightly-packed RGBA8 to I420 (YUV 4:2:0 planar) using BT.601
// limited range — the inverse of i420ToRgba in yuv_to_rgb.h.
//
// Width and height must be even. The U and V planes are produced by
// box-averaging each 2x2 RGB block then applying the BT.601 matrix.
// Plane buffers must be sized:
//   Y: strideY * height
//   U: strideU * (height/2)
//   V: strideV * (height/2)
// Pass strideY/U/V == 0 to use tight strides (width / width/2 / width/2).
void rgbaToI420(const uint8_t* rgba, int srcStride,
                int width, int height,
                uint8_t* y, int strideY,
                uint8_t* u, int strideU,
                uint8_t* v, int strideV);

} // namespace bro::video
