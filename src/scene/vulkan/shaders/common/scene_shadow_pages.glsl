// The cached shadow pages' addressing (docs/scene-world-architecture.md §8),
// shared by the marking pass (shadow/shadow_mark.comp) and every lit
// shader's lookup (common/scene_lighting.glsl): a virtual map as the frame
// samples it, and which page-table entry holds the page under a point.
// Mirrors SceneShadowMapUniform (scene_vk_descriptors.h); the numbers are
// shadow/shadow_pages_layout.h's.

#include "shadow/shadow_pages_layout.h"

// A virtual map: a sun's clipmap level (its window of SP_WINDOW pages a
// side) or a local light's map or face (a mip chain, SP_LOCAL pages a side
// at its finest). `matrix` takes a camera-relative position to (map uv,
// [0,1] depth); info = (first page-table entry, kind, a clipmap window's
// origin page mod SP_WINDOW in x, y); bias = (constant depth bias, normal
// offset, a clipmap level's texel (m), a local map's finest texel a metre
// from the light); depth = (near, far, ortho, clipmap level).
struct ShadowMapGpu {
    mat4 matrix;
    vec4 info;
    vec4 bias;
    vec4 depth;
};

// The entry of clipmap map `map`'s page `page` (in its window, 0 to
// SP_WINDOW - 1): the window is addressed toroidally from its origin.
int shadowClipmapEntry(ShadowMapGpu map, ivec2 page) {
    ivec2 slot = (ivec2(map.info.zw) + page) & (SP_WINDOW - 1);
    return int(map.info.x) + slot.y * SP_WINDOW + slot.x;
}

// The entry of local map `map`'s mip `mip` page `page`.
int shadowLocalEntry(ShadowMapGpu map, int mip, ivec2 page) {
    int n = SP_LOCAL >> mip;
    return int(map.info.x) + SP_LOCAL_MIP_FIRST(mip) + page.y * n + page.x;
}

// A point light's cube face for the direction `toFrag` from it (the planner's
// face order: +x, -x, +y, -y, +z, -z).
int shadowCubeFace(vec3 toFrag) {
    vec3 a = abs(toFrag);
    if (a.x >= a.y && a.x >= a.z) return toFrag.x > 0.0 ? 0 : 1;
    if (a.y >= a.z) return toFrag.y > 0.0 ? 2 : 3;
    return toFrag.z > 0.0 ? 4 : 5;
}
