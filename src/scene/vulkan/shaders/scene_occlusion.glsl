// Occlusion against the frame's HiZ (SceneTargets::hiZ, built by PassHiZ):
// the farthest depth drawn so far, each level half the one below, level 0
// half the frame. A box whose nearest point lies behind every depth drawn
// over its screen rectangle would fail the depth test at every pixel, so a
// draw may skip it whole; the test is conservative (the rectangle is
// widened by a pixel, the texels are the farthest of all under them), so
// what it skips is exactly what the depth test would have dropped and the
// image is the same.
//
// The includer declares REVERSED_Z (constant_id 0, the camera's depth
// policy) and the pyramid as `texHiZ` (a sampler2D); by default it is the
// lighting set's binding 10 (scene_vk_descriptors.cpp), read in the vertex
// stage, which a mesh pipeline gets. A compute pass binds it where it likes
// with SCENE_HIZ_SET / SCENE_HIZ_BINDING. sceneHiZOccludedRect takes a rect
// in the frame's pixels; sceneHiZOccludedClip four clip-space corners of a
// planar card or the eight of a box (count), and the frame's size.

#ifndef SCENE_HIZ_SET
#define SCENE_HIZ_SET 1
#define SCENE_HIZ_BINDING 10
#endif
layout(set = SCENE_HIZ_SET, binding = SCENE_HIZ_BINDING) uniform sampler2D texHiZ;

float sceneHiZFarther(float a, float b) { return REVERSED_Z ? min(a, b) : max(a, b); }

// Pixels [pxMin, pxMax] (inclusive, the frame's) all lie behind what was
// drawn, at depth `nearest` (the box's nearest, in the depth buffer's terms).
bool sceneHiZOccludedRect(ivec2 pxMin, ivec2 pxMax, float nearest) {
    ivec2 t0 = pxMin >> 1, t1 = pxMax >> 1;
    int e = max(t1.x - t0.x, t1.y - t0.y);
    // The level at which the rect spans at most two texels each way.
    int level = e > 0 ? findMSB(e) : 0;
    if (any(greaterThan((t1 >> level) - (t0 >> level), ivec2(1)))) ++level;
    level = min(level, textureQueryLevels(texHiZ) - 1);
    ivec2 dim = textureSize(texHiZ, level);
    // (Past a level's last texel is still that texel: PassHiZ folds an odd
    // row or column into the last one.)
    ivec2 a = min(t0 >> level, dim - 1), b = min(t1 >> level, dim - 1);
    float far = texelFetch(texHiZ, a, level).r;
    far = sceneHiZFarther(far, texelFetch(texHiZ, ivec2(b.x, a.y), level).r);
    far = sceneHiZFarther(far, texelFetch(texHiZ, ivec2(a.x, b.y), level).r);
    far = sceneHiZFarther(far, texelFetch(texHiZ, b, level).r);
    return REVERSED_Z ? nearest < far : nearest > far;
}

// The shape with clip-space corners c[0 .. count) (a planar card's four, a
// box's eight) is hidden. Never for a corner behind the eye or past the
// near or far plane (the frustum's business), nor wholly off the frame.
bool sceneHiZOccludedClip(vec4 c[8], int count, vec2 frame) {
    vec2 lo = vec2(1e30), hi = vec2(-1e30);
    float nearest = REVERSED_Z ? 0.0 : 1.0;
    for (int i = 0; i < count; ++i) {
        if (c[i].w <= 1e-6) return false;
        vec3 n = c[i].xyz / c[i].w;
        if (n.z < 0.0 || n.z > 1.0) return false;
        vec2 p = (n.xy * 0.5 + 0.5) * frame;
        lo = min(lo, p);
        hi = max(hi, p);
        nearest = REVERSED_Z ? max(nearest, n.z) : min(nearest, n.z);
    }
    if (hi.x < 0.0 || hi.y < 0.0 || lo.x >= frame.x || lo.y >= frame.y) return false;
    ivec2 size = ivec2(frame);
    ivec2 pxMin = clamp(ivec2(floor(lo)) - 1, ivec2(0), size - 1);
    ivec2 pxMax = clamp(ivec2(floor(hi)) + 1, ivec2(0), size - 1);
    return sceneHiZOccludedRect(pxMin, pxMax, nearest);
}
