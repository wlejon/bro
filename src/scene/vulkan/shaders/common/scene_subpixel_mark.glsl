// The opaque scope's sub-pixel mark (SceneTargets::mark, R8), which the
// sub-pixel resolve reads (scene/vulkan/post/pass_subpixel.h). A pixel is
// one of:
//   0                  the ground the cards stand on (and the sky): it takes
//                      its marked neighbours' blend, being what their
//                      dither left showing;
//   subpixelMark(s)    a surface of its own, kept as it is but for `s`, the
//                      share of it that is a dither's all-or-nothing
//                      decision on detail finer than a pixel (0: a solid
//                      leaf, a blade, a trunk; 1: a far element's speck).

// How far a cut card's pixel kept at `cover` (coverSampleMask) was the
// dither's all-or-nothing decision: any part cover, flipping as it moves
// over the pixels; a whole one is not.
float subpixelDecided(float cover) {
    return 1.0 - smoothstep(0.85, 1.0, cover);
}

float subpixelMark(float share) {
    return (2.0 + 253.0 * clamp(share, 0.0, 1.0)) / 255.0;
}

bool subpixelIsGround(float mark) {
    return mark < 1.0 / 255.0;
}

float subpixelShare(float mark) {
    return max(mark * 255.0 - 2.0, 0.0) / 253.0;
}
