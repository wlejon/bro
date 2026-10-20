// Occlusion against the frame's HiZ (SceneTargets::hiZ, built by PassHiZ):
// the farthest depth drawn so far, each level half the one below, level 0
// half the frame. A box whose nearest point lies behind every depth drawn
// over its screen rectangle would fail the depth test at every pixel, so a
// draw may skip it whole; the test is conservative (the rectangle is
// widened by a pixel, the texels are the farthest of all under them), so
// what it skips is exactly what the depth test would have dropped and the
// image is the same.
//
// Depth is reversed (near 1, far 0: scene/depth_policy.h). The includer
// declares the pyramid as `texHiZ` (a sampler2D); by default it is the
// lighting set's binding 10 (scene_vk_descriptors.cpp), read in the vertex
// stage, which a mesh pipeline gets. A compute pass binds it where it likes
// with SCENE_HIZ_SET / SCENE_HIZ_BINDING. sceneHiZOccludedRect takes a rect
// in the frame's pixels; sceneHiZOccludedClip four clip-space corners of a
// planar card or the eight of a box (count), and the frame's size.

//
// Two-phase (PassOpaque's retest): texHiZPrev is the last frame's pyramid
// (binding 10) and texHiZ this frame's (binding 11, built from the opaque
// batches' depth). The batches hold back what the last frame's hides
// (reprojected: camera.occlusionPrev) and list it (binding 12,
// occlusion_list_layout.h); PassHiZ tests the list against this frame's,
// and the retest draws what it does not hide. The batches' depth is a part
// of the frame's, so whatever the frame's would show is drawn by one or the
// other.

#ifndef SCENE_HIZ_SET
#define SCENE_HIZ_SET 1
#define SCENE_HIZ_BINDING 11
#define SCENE_HIZ_PREV_BINDING 10
#endif
layout(set = SCENE_HIZ_SET, binding = SCENE_HIZ_BINDING) uniform sampler2D texHiZ;
#ifdef SCENE_HIZ_PREV_BINDING
layout(set = SCENE_HIZ_SET, binding = SCENE_HIZ_PREV_BINDING) uniform sampler2D texHiZPrev;
#endif

float sceneHiZFarther(float a, float b) { return min(a, b); }

// Pixels [pxMin, pxMax] (inclusive, the frame's) all lie behind what pyramid
// `hiZ` says was drawn, at depth `nearest` (the box's nearest, in the depth
// buffer's terms).
bool sceneHiZOccludedRectIn(sampler2D hiZ, ivec2 pxMin, ivec2 pxMax, float nearest) {
    ivec2 t0 = pxMin >> 1, t1 = pxMax >> 1;
    int e = max(t1.x - t0.x, t1.y - t0.y);
    // The level at which the rect spans at most two texels each way.
    int level = e > 0 ? findMSB(e) : 0;
    if (any(greaterThan((t1 >> level) - (t0 >> level), ivec2(1)))) ++level;
    level = min(level, textureQueryLevels(hiZ) - 1);
    ivec2 dim = textureSize(hiZ, level);
    // (Past a level's last texel is still that texel: PassHiZ folds an odd
    // row or column into the last one.)
    ivec2 a = min(t0 >> level, dim - 1), b = min(t1 >> level, dim - 1);
    float far = texelFetch(hiZ, a, level).r;
    far = sceneHiZFarther(far, texelFetch(hiZ, ivec2(b.x, a.y), level).r);
    far = sceneHiZFarther(far, texelFetch(hiZ, ivec2(a.x, b.y), level).r);
    far = sceneHiZFarther(far, texelFetch(hiZ, b, level).r);
    return nearest < far;
}

bool sceneHiZOccludedRect(ivec2 pxMin, ivec2 pxMax, float nearest) {
    return sceneHiZOccludedRectIn(texHiZ, pxMin, pxMax, nearest);
}

// The pixel rect (widened by a pixel) and nearest depth a pyramid tests of
// the shape with clip-space corners c[0 .. count) (a planar card's four, a
// box's eight); false when no pyramid may hide it: a corner behind the eye
// or past the near or far plane (the frustum's business), or wholly off the
// frame.
bool sceneHiZClipRect(vec4 c[8], int count, vec2 frame, out ivec2 pxMin, out ivec2 pxMax, out float nearest) {
    vec2 lo = vec2(1e30), hi = vec2(-1e30);
    nearest = 0.0;
    pxMin = pxMax = ivec2(0);
    for (int i = 0; i < count; ++i) {
        if (c[i].w <= 1e-6) return false;
        vec3 n = c[i].xyz / c[i].w;
        if (n.z < 0.0 || n.z > 1.0) return false;
        vec2 p = (n.xy * 0.5 + 0.5) * frame;
        lo = min(lo, p);
        hi = max(hi, p);
        nearest = max(nearest, n.z);
    }
    if (hi.x < 0.0 || hi.y < 0.0 || lo.x >= frame.x || lo.y >= frame.y) return false;
    ivec2 size = ivec2(frame);
    pxMin = clamp(ivec2(floor(lo)) - 1, ivec2(0), size - 1);
    pxMax = clamp(ivec2(floor(hi)) + 1, ivec2(0), size - 1);
    return true;
}

// The shape with clip-space corners c[0 .. count) is hidden by pyramid `hiZ`.
bool sceneHiZOccludedClipIn(sampler2D hiZ, vec4 c[8], int count, vec2 frame) {
    ivec2 pxMin, pxMax;
    float nearest;
    return sceneHiZClipRect(c, count, frame, pxMin, pxMax, nearest) &&
           sceneHiZOccludedRectIn(hiZ, pxMin, pxMax, nearest);
}

bool sceneHiZOccludedClip(vec4 c[8], int count, vec2 frame) {
    return sceneHiZOccludedClipIn(texHiZ, c, count, frame);
}

#ifdef SCENE_HIZ_PREV_BINDING
#include "occlusion_list_layout.h"
layout(std430, set = SCENE_HIZ_SET, binding = 12) buffer OcclusionList { uint olWords[]; };

// The first phase for a shape with camera-relative corners p[0 .. count) of
// `slot`'s draw (-1: none), instance `instance` from vertex `firstVertex`:
// whether the batches hold it back, the last frame's pyramid hiding it
// (reprojected). Every vertex of the shape asks, and one (`appends`) adds
// its entry, with its rect in this frame, for the retest's test.
bool sceneOcclusionHoldsBack(vec3 p[8], int count, vec2 frame, int slot, uint instance, uint firstVertex,
                             bool appends) {
    if (camera.occlusion.z < 0.5 || slot < 0 || olWords[OL_ENABLED + slot] == 0u) return false;
    vec4 c[8];
    for (int i = 0; i < count; ++i) c[i] = camera.occlusionPrev * vec4(p[i], 1.0);
    if (!sceneHiZOccludedClipIn(texHiZPrev, c, count, frame)) return false;
    if (appends) {
        for (int i = 0; i < count; ++i) c[i] = camera.viewProj * vec4(p[i], 1.0);
        ivec2 pxMin, pxMax;
        float nearest;
        bool testable = sceneHiZClipRect(c, count, frame, pxMin, pxMax, nearest);
        // (The enabled slots' cards all fit: an entry for every card held back.)
        uint at = atomicAdd(olWords[OL_TOTAL], 1u);
        atomicAdd(olWords[OL_HELD + slot], 1u);
        uint e = OL_ENTRIES + at * OL_ENTRY_WORDS;
        olWords[e] = uint(slot);
        olWords[e + 1] = instance;
        olWords[e + 2] = firstVertex;
        olWords[e + 3] = floatBitsToUint(nearest);
        olWords[e + 4] = testable ? uint(pxMin.x) | (uint(pxMin.y) << 16) : OL_SHOWN;
        olWords[e + 5] = uint(pxMax.x) | (uint(pxMax.y) << 16);
    }
    return true;
}

// The retest's card: the instance and first vertex of its draw's k-th
// (gl_InstanceIndex) card this frame's pyramid does not hide.
uvec2 sceneOcclusionRetestCard(int k) {
    return uvec2(olWords[OL_SORTED + 2 * k], olWords[OL_SORTED + 2 * k + 1]);
}
#endif
