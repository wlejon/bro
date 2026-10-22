// The land's tiles (scene/nodes/land_node.h): its heights, surface and
// cover, a tile of each level in the slots of three arrays, found through
// the page table as every surface layer's are (world/surface_layers.h,
// SurfaceLayers::encodePage). Where the land is drawn the bindings are set
// 3's (scene_mesh_drawer_land.cpp's prepareLand); a grass node standing on
// the land (scene_grass.glsl) declares its own, with its cover, and defines
// LAND_W(i), the i-th vec4 of its LandDraw words, before including this.

#include "ground/scene_land_layout.h"

#ifndef LAND_W
layout(set = 3, binding = 0) uniform LandDrawUBO { vec4 w[LD_WORDS / 4]; } landDraw;
layout(set = 3, binding = 4) uniform sampler2DArray landHeights;
layout(set = 3, binding = 5) uniform sampler2DArray landSurfaces;
layout(set = 3, binding = 6) uniform usampler2DArray landPage;
layout(set = 3, binding = 7) uniform sampler2DArray landCanopies;
#define LAND_W(i) landDraw.w[i]
#define LAND_CANOPY
#endif

float landExtent() { return LAND_W(LD_LAND / 4).x; }
vec2 landEye() { return LAND_W(LD_LAND / 4).yz; }
vec3 landEye3() { return LAND_W(LD_LAND / 4).ywz; }
float landCell0() { return LAND_W(LD_RINGS / 4).x; }
float landInvK() { return LAND_W(LD_RINGS / 4).y; }
float landRingHalf() { return LAND_W(LD_RINGS / 4).w; }

// The ring cell wanted at `xz` (mesh_land.vert). From the frame's eye,
// landEye, in every pass: a probe's or a turf tile's view, and the grass
// standing on the land, read the land the frame does.
float landCell(vec2 xz) {
    vec2 d = abs(xz - landEye());
    return max(landCell0(), max(d.x, d.y) * landInvK());
}

// The level whose texel is half the cell wanted (a level-0 texel is half
// the finest cell, LandNode::tileConfig).
float landLevel(vec2 xz) { return log2(landCell(xz) / landCell0()); }

vec2 landClamp(vec2 xz) { return clamp(xz, vec2(-landExtent()), vec2(landExtent())); }

// The slot of the tile of `level` holding `xz`, and `uv` there; -1 when no
// tile there is resident.
int landSlot(vec2 xz, int level, out vec2 uv) {
    vec4 tiles = LAND_W(LD_TILES / 4), page = LAND_W(LD_PAGE / 4);
    float side = tiles.x * exp2(float(level));
    vec2 t = xz / side;
    ivec2 ti = ivec2(floor(t));
    int n = int(page.y);
    uint e = texelFetch(landPage, ivec3(ti & (n - 1), level), 0).r;
    uv = vec2(0.0);
    if (e == 0u) return -1;
    uvec2 hi = uvec2(ti >> int(page.x)) & 1023u;
    if (((e >> 12u) & 1023u) != hi.x || (e >> 22u) != hi.y) return -1;
    uv = (tiles.z + (t - vec2(ti)) * tiles.y) / (tiles.y + 2.0 * tiles.z);
    return int(e & 4095u) - 1;
}

// Where the land at `xz` is read for level `lambda` (fractional: blended
// toward the next coarser across the level), or the finest coarser level
// resident when that one is not: the slots and uvs of the two levels and
// the blend (s1 < 0: one level). False when no level is (before the first
// fill).
bool landTexels(vec2 xz, float lambda, out int s0, out vec2 uv0, out int s1, out vec2 uv1, out float f,
                out int level) {
    s0 = s1 = -1;
    uv0 = uv1 = vec2(0.0);
    f = 0.0;
    level = 0;
    int levels = int(LAND_W(LD_TILES / 4).w + 0.5);
    if (levels <= 0 || LAND_W(LD_PAGE / 4).w < 0.5) return false;
    lambda = clamp(lambda, 0.0, float(levels - 1));
    int l0 = int(lambda);
    level = l0;
    for (; level < levels; ++level) {
        s0 = landSlot(xz, level, uv0);
        if (s0 >= 0) break;
    }
    if (s0 < 0) return false;
    f = lambda - float(l0);
    if (level == l0 && f > 0.0 && l0 + 1 < levels) s1 = landSlot(xz, l0 + 1, uv1);
    return true;
}
bool landTexels(vec2 xz, float lambda, out int s0, out vec2 uv0, out int s1, out vec2 uv1, out float f) {
    int level;
    return landTexels(xz, lambda, s0, uv0, s1, uv1, f, level);
}

// The height at `uv` of slot `s` and its slope (dh/dx, dh/dz) from the
// heights a texel either way (inside the tile: its border is two texels),
// the tile's level `level`.
float landSlope(int s, vec2 uv, int level, out vec2 slope) {
    vec4 tiles = LAND_W(LD_TILES / 4);
    float du = 1.0 / (tiles.y + 2.0 * tiles.z);
    float texel = tiles.x * exp2(float(level)) / tiles.y;
    float l = float(s);
    float hx0 = textureLod(landHeights, vec3(uv - vec2(du, 0.0), l), 0.0).r;
    float hx1 = textureLod(landHeights, vec3(uv + vec2(du, 0.0), l), 0.0).r;
    float hz0 = textureLod(landHeights, vec3(uv - vec2(0.0, du), l), 0.0).r;
    float hz1 = textureLod(landHeights, vec3(uv + vec2(0.0, du), l), 0.0).r;
    slope = vec2(hx1 - hx0, hz1 - hz0) / (2.0 * texel);
    return textureLod(landHeights, vec3(uv, l), 0.0).r;
}


// The land at `xz` from the tiles of level `lambda` (landTexels): its height
// and its surface (rgb its albedo, a its herbs). False when no level is
// resident.
bool landSample(vec2 xz, float lambda, out float height, out vec4 surface) {
    height = 0.0;
    surface = vec4(0.18, 0.18, 0.18, 0.0);
    int s0, s1;
    vec2 uv0, uv1;
    float f;
    if (!landTexels(xz, lambda, s0, uv0, s1, uv1, f)) return false;
    height = textureLod(landHeights, vec3(uv0, float(s0)), 0.0).r;
    surface = textureLod(landSurfaces, vec3(uv0, float(s0)), 0.0);
    if (s1 >= 0) {
        height = mix(height, textureLod(landHeights, vec3(uv1, float(s1)), 0.0).r, f);
        surface = mix(surface, textureLod(landSurfaces, vec3(uv1, float(s1)), 0.0), f);
    }
    return true;
}

// The land at `xz` from the tiles of level `lambda` (landTexels) with its
// normal (from the heights a texel of the level read either way, landSlope,
// as the fill's) and its surface. Level ground where no level is resident.
bool landSampleNormal(vec2 xz, float lambda, out float height, out vec3 normal, out vec4 surface) {
    height = 0.0;
    normal = vec3(0.0, 1.0, 0.0);
    surface = vec4(0.18, 0.18, 0.18, 0.0);
    int s0, s1, level;
    vec2 uv0, uv1, g;
    float f;
    if (!landTexels(xz, lambda, s0, uv0, s1, uv1, f, level)) return false;
    height = landSlope(s0, uv0, level, g);
    surface = textureLod(landSurfaces, vec3(uv0, float(s0)), 0.0);
    if (s1 >= 0) {
        vec2 g1;
        height = mix(height, landSlope(s1, uv1, level + 1, g1), f);
        g = mix(g, g1, f);
        surface = mix(surface, textureLod(landSurfaces, vec3(uv1, float(s1)), 0.0), f);
    }
    normal = normalize(vec3(-g.x, 1.0, -g.y));
    return true;
}

#ifdef LAND_COVERS
// All of the land at `xz` (landSampleNormal's height, normal and surface)
// with what covers it (LandSurface::cover, after the fill's
// clamps): x the sward's density, y its dryness, z its height multiplier,
// w the canopy. Bare level ground where no level is resident.
bool landSampleCover(vec2 xz, float lambda, out float height, out vec3 normal, out vec4 surface, out vec4 cover) {
    height = 0.0;
    normal = vec3(0.0, 1.0, 0.0);
    surface = vec4(0.18, 0.18, 0.18, 0.0);
    cover = vec4(0.0, 0.0, 1.0, 0.0);
    int s0, s1, level;
    vec2 uv0, uv1, g;
    float f;
    if (!landTexels(xz, lambda, s0, uv0, s1, uv1, f, level)) return false;
    height = landSlope(s0, uv0, level, g);
    surface = textureLod(landSurfaces, vec3(uv0, float(s0)), 0.0);
    cover = textureLod(landCovers, vec3(uv0, float(s0)), 0.0);
    if (s1 >= 0) {
        vec2 g1;
        height = mix(height, landSlope(s1, uv1, level + 1, g1), f);
        g = mix(g, g1, f);
        surface = mix(surface, textureLod(landSurfaces, vec3(uv1, float(s1)), 0.0), f);
        cover = mix(cover, textureLod(landCovers, vec3(uv1, float(s1)), 0.0), f);
    }
    normal = normalize(vec3(-g.x, 1.0, -g.y));
    return true;
}
#endif

#ifdef LAND_CANOPY
// The plants' canopy (world/plant_canopy.h): how far from the eye they are
// drawn and cast, and whether the land carries their canopy at all.
float landCanopyRange() { return LAND_W(LD_CANOPY / 4).x; }
float landCanopyCastRange() { return LAND_W(LD_CANOPY / 4).y; }
bool landHasCanopy() { return LAND_W(LD_CANOPY / 4).z > 0.5; }

// How much of the plants the canopy stands for at `d` m from the eye past
// `range`: none inside its last tenth, all of it at the range (as the
// plants dither out over that tenth, plants_plan.comp).
float landCanopyShown(float d, float range) { return smoothstep(0.9 * range, range, d); }

// The canopy at `xz` from the tiles of level `lambda`: rgb its leaves'
// albedo, a its cover; `top` its top over the ground. None where no level
// is resident.
vec4 landSampleCanopy(vec2 xz, float lambda, out float top) {
    top = 0.0;
    int s0, s1;
    vec2 uv0, uv1;
    float f;
    if (!landTexels(xz, lambda, s0, uv0, s1, uv1, f)) return vec4(0.0);
    vec4 c = textureLod(landCanopies, vec3(uv0, float(s0)), 0.0);
    top = textureLod(landHeights, vec3(uv0, float(s0)), 0.0).g;
    if (s1 >= 0) {
        c = mix(c, textureLod(landCanopies, vec3(uv1, float(s1)), 0.0), f);
        top = mix(top, textureLod(landHeights, vec3(uv1, float(s1)), 0.0).g, f);
    }
    return c;
}
#endif

// The land's height at `xz`, read at its level.
float landHeightAt(vec2 xz) {
    float h;
    vec4 s;
    landSample(xz, landLevel(xz), h, s);
    return h;
}
