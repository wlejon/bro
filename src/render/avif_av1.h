#pragma once

// AVIF's AV1 decoder: dav1d (BSD-2, from vcpkg), registered with broimage
// (broimage/heif.h set_av1_decoder). broimage reads the HEIF container,
// grids, alpha and transforms and converts the YUV to RGBA itself; this is
// only the bitstream decode. Called by registerImageCodecs (image_codecs.h)
// in a build with BRO_WITH_AVIF.

namespace bro::render {

void registerAvifDecoder();

}  // namespace bro::render
