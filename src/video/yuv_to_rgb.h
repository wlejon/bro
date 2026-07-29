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
//
// `rotationDegrees` is TrackInfo::rotationDegrees: how far the stored frame
// has to be turned CLOCKWISE to be the right way up. It is applied here, so
// dstW/dstH are the size of the picture the RIGHT WAY UP — at a quarter turn
// that is the swapped pair, and a caller asking for a 128x72 box out of a
// sideways phone clip wants 72x128. Anything that is not a quarter turn is
// treated as no rotation, matching every reader that fills the field.
//
// Applied to the SAMPLING box rather than by a second pass over the output:
// each destination pixel is an unweighted average of a source rectangle, and
// an average does not care which order it walks, so turning the rectangle is
// exact. Rotating the scaled image afterwards would be a second resampling
// and would soften every thumbnail on a phone clip to buy nothing.
void i420ToRgbaScaled(const uint8_t* y, const uint8_t* u, const uint8_t* v,
                      int strideY, int strideU, int strideV,
                      int srcW, int srcH,
                      uint8_t* dst, int dstStride, int dstW, int dstH,
                      int rotationDegrees);

} // namespace bro::video
