// The probe grid's block and its lookups (scene/vulkan/lighting/
// scene_probe_grid.h; the layout lighting/probe_grid_layout.h), shared by
// its compute stages and every lit shader. Positions are camera-relative,
// as in every scene shader.
//
// Included twice: first for the struct (before the block that holds one is
// declared), then, once the includer has defined PG (the block, a
// ProbeGridGpu) and PG_HEIGHT_LOAD(texel) (the ground's height image at an
// ivec2 texel: a world height, or under PG_HOLE_TEST where nothing was
// captured), for the lookups.

#ifndef PG_STRUCT_DEFINED
#define PG_STRUCT_DEFINED
#include "lighting/probe_grid_layout.h"

struct ProbeGridGpu {
    vec4 params;                     // x on, y cascades, z the layers' top above the ground (m), w the lowest layer (m)
    vec4 eye;                        // xyz the eye in the world (the records' frame), w the layers' power
    vec4 sun;                        // xyz toward the sun, w 0
    vec4 cascade[PG_MAX_CASCADES];   // xy the corner of its world (x, z), z column spacing, w 1 / spacing
    vec4 wrap[PG_MAX_CASCADES];      // xy where column 0 is stored (its corner column mod PG_COLUMNS)
};
#endif

#if defined(PG) && !defined(PG_LOOKUPS_DEFINED)
#define PG_LOOKUPS_DEFINED

int pgCascades() { return int(PG.params.y); }
float pgTop() { return PG.params.z; }

// Cascade c's world spans its corner to PG_COLUMNS columns past it.
bool pgInside(int c, vec2 xz) {
    vec2 g = (xz - PG.cascade[c].xy) * PG.cascade[c].w;
    return all(greaterThanEqual(g, vec2(0.0))) && all(lessThan(g, vec2(float(PG_COLUMNS))));
}

// The finest cascade from `first` whose world holds `xz`; -1 past them all.
int pgCascadeAt(vec2 xz, int first) {
    for (int c = first; c < pgCascades(); ++c)
        if (pgInside(c, xz)) return c;
    return -1;
}

// The ground's height (camera-relative) under `xz` in cascade c: bilinear
// over its texels, the holes left out; PG_HOLE where all four are holes.
float pgGround(int c, vec2 xz) {
    vec2 t = (xz - PG.cascade[c].xy) * PG.cascade[c].w * float(PG_HEIGHT / PG_COLUMNS) - 0.5;
    t = clamp(t, vec2(0.0), vec2(float(PG_HEIGHT - 1)));
    ivec2 i0 = ivec2(floor(t));
    ivec2 i1 = min(i0 + 1, ivec2(PG_HEIGHT - 1));
    vec2 f = t - vec2(i0);
    int row = c * PG_HEIGHT;
    float h[4] = float[4](PG_HEIGHT_LOAD(ivec2(i0.x, row + i0.y)), PG_HEIGHT_LOAD(ivec2(i1.x, row + i0.y)),
                          PG_HEIGHT_LOAD(ivec2(i0.x, row + i1.y)), PG_HEIGHT_LOAD(ivec2(i1.x, row + i1.y)));
    float w[4] = float[4]((1.0 - f.x) * (1.0 - f.y), f.x * (1.0 - f.y), (1.0 - f.x) * f.y, f.x * f.y);
    float sum = 0.0, wsum = 0.0;
    for (int k = 0; k < 4; ++k) {
        if (h[k] < PG_HOLE_TEST) continue;
        sum += h[k] * w[k];
        wsum += w[k];
    }
    return wsum > 1e-6 ? sum / wsum - PG.eye.y : PG_HOLE;
}

bool pgHole(float ground) { return ground < PG_HOLE_TEST; }

// Where a column stands on a hole: the eye's own ground, taken as 1.7 m
// below it (a world with no ground under some of it still has its probes).
float pgBase(float ground) { return pgHole(ground) ? -1.7 : ground; }

// The canopy's volumes at camera-relative (x, z) and `h` above the ground
// there, in cascade c: the 3D images' coordinates, clamped inside the
// cascade's slab so a filter never reads the next cascade's.
vec3 pgVolumeUvw(int c, vec2 xz, float h) {
    vec2 uv = (xz - PG.cascade[c].xy) * PG.cascade[c].w * (1.0 / float(PG_COLUMNS));
    float k = clamp(h / pgTop() * float(PG_LAYERS_Y), 0.5, float(PG_LAYERS_Y) - 0.5);
    return vec3(uv, (float(c * PG_LAYERS_Y) + k) / float(PG_LAYERS_Y * PG_MAX_CASCADES));
}

// Layer k's height above the ground, and its inverse (scene/world/
// probe_grid.h's layerHeight and layerAt).
float pgLayerHeight(float k) {
    return PG.params.w + (pgTop() - PG.params.w) * pow(k / float(PG_LAYERS - 1), PG.eye.w);
}
float pgLayerAt(float h) {
    float t = clamp((h - PG.params.w) / (pgTop() - PG.params.w), 0.0, 1.0);
    return pow(t, 1.0 / PG.eye.w) * float(PG_LAYERS - 1);
}

// The probes' SH images at camera-relative (x, z) and `h` above the ground,
// in cascade c, for a sampler that repeats across (the columns wrap): the
// columns clamped to the square's (a filter never reads a column wrapped
// in from its far side), the layers to the cascade's slab.
vec3 pgProbeUvw(int c, vec2 xz, float h) {
    vec2 g = clamp((xz - PG.cascade[c].xy) * PG.cascade[c].w - 0.5, vec2(0.0), vec2(float(PG_COLUMNS - 1)));
    float k = float(c * PG_LAYERS) + pgLayerAt(h) + 0.5;
    return vec3((PG.wrap[c].xy + g + 0.5) / float(PG_COLUMNS), k / float(PG_LAYERS * PG_MAX_CASCADES));
}

// The irradiance over pi on a surface facing `n` from a probe's SH (one
// texel a channel), and the mean radiance about it.
vec3 pgShIrradiance(vec4 r, vec4 g, vec4 b, vec3 n) {
    vec4 basis = vec4(PG_SH_Y00, (2.0 / 3.0) * PG_SH_Y1 * n);
    return max(vec3(dot(r, basis), dot(g, basis), dot(b, basis)), vec3(0.0));
}
vec3 pgShMean(vec4 r, vec4 g, vec4 b) { return max(vec3(r.x, g.x, b.x) * PG_SH_Y00, vec3(0.0)); }
#endif
