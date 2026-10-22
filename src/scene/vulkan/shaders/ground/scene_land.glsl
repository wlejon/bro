// The land's tiles where it is drawn (scene/nodes/land_node.h): its heights
// and surface, a tile of each level in the slots of two arrays, found
// through the page table as every surface layer's are
// (world/surface_layers.h, SurfaceLayers::encodePage). The bindings are set
// 3's (scene_mesh_drawer_land.cpp's landSet).

#include "ground/scene_land_layout.h"

layout(set = 3, binding = 0) uniform LandDrawUBO { vec4 w[LD_WORDS / 4]; } landDraw;
layout(set = 3, binding = 4) uniform sampler2DArray landHeights;
layout(set = 3, binding = 5) uniform sampler2DArray landSurfaces;
layout(set = 3, binding = 6) uniform usampler2DArray landPage;

float landExtent() { return landDraw.w[LD_LAND / 4].x; }
vec2 landEye() { return landDraw.w[LD_LAND / 4].yz; }
float landCell0() { return landDraw.w[LD_RINGS / 4].x; }
float landInvK() { return landDraw.w[LD_RINGS / 4].y; }
float landRingHalf() { return landDraw.w[LD_RINGS / 4].w; }

// The slot of the tile of `level` holding `xz`, and `uv` there; -1 when no
// tile there is resident.
int landSlot(vec2 xz, int level, out vec2 uv) {
    vec4 tiles = landDraw.w[LD_TILES / 4], page = landDraw.w[LD_PAGE / 4];
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

// The land at `xz` from the tiles of level `lambda` (fractional: blended
// toward the next coarser across the level), or the finest coarser level
// resident when that one is not: its height and its surface (rgb its
// albedo). False when no level is (before the first fill).
bool landSample(vec2 xz, float lambda, out float height, out vec4 surface) {
    height = 0.0;
    surface = vec4(0.18, 0.18, 0.18, 1.0);
    int levels = int(landDraw.w[LD_TILES / 4].w + 0.5);
    if (levels <= 0 || landDraw.w[LD_PAGE / 4].w < 0.5) return false;
    lambda = clamp(lambda, 0.0, float(levels - 1));
    int l0 = int(lambda);
    float f = lambda - float(l0);
    vec2 uv;
    int slot = -1, level = l0;
    for (; level < levels; ++level) {
        slot = landSlot(xz, level, uv);
        if (slot >= 0) break;
    }
    if (slot < 0) return false;
    height = textureLod(landHeights, vec3(uv, float(slot)), 0.0).r;
    surface = textureLod(landSurfaces, vec3(uv, float(slot)), 0.0);
    if (level == l0 && f > 0.0 && l0 + 1 < levels) {
        vec2 uv1;
        int s1 = landSlot(xz, l0 + 1, uv1);
        if (s1 >= 0) {
            height = mix(height, textureLod(landHeights, vec3(uv1, float(s1)), 0.0).r, f);
            surface = mix(surface, textureLod(landSurfaces, vec3(uv1, float(s1)), 0.0), f);
        }
    }
    return true;
}
