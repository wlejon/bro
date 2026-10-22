// The land's tiles (scene/nodes/land_node.h): its heights, surface and
// cover, a tile of each level in the slots of three arrays, found through
// the page table as every surface layer's are (world/surface_layers.h,
// SurfaceLayers::encodePage). Where the land is drawn the bindings are set
// 3's (scene_mesh_drawer_land.cpp's prepareLand); a grass node standing on
// the land (scene_grass.glsl) declares its own, with its cover, and defines
// LAND_W(i), the i-th vec4 of its LandDraw words, before including this.
//
// Where is the land's MAP (scene_land_layout.h): a flat body's x and z, a
// round body's azimuthal map about the anchor. A map point's tiles are its
// chart's (landPosOf), and landPoint stands it in the land's frame.

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

float landReach() { return LAND_W(LD_LAND / 4).x; }
vec2 landEye() { return LAND_W(LD_LAND / 4).yz; }
vec3 landEye3() { return LAND_W(LD_EYE / 4).xyz; }
float landCell0() { return LAND_W(LD_RINGS / 4).x; }
float landInvK() { return LAND_W(LD_RINGS / 4).y; }
float landRingHalf() { return LAND_W(LD_RINGS / 4).w; }
float landRadius() { return LAND_W(LD_BODY / 4).x; }
bool landRound() { return LAND_W(LD_BODY / 4).x > 0.0; }
// The ring ring 0 is drawn as (LandNode::firstRing): the finer ones are
// under a pixel from the eye's height.
float landFirstRing() { return LAND_W(LD_BODY / 4).y; }

// --- the map ---

vec2 landClamp(vec2 m) {
    if (landRound()) {
        float r = length(m);
        return r > landReach() ? m * (landReach() / r) : m;
    }
    return clamp(m, vec2(-landReach()), vec2(landReach()));
}

// The body's up at map point `m` in the land's frame.
vec3 landUp(vec2 m) {
    if (!landRound()) return vec3(0.0, 1.0, 0.0);
    float r = length(m);
    float th = r / landRadius();
    vec2 dir = r > 0.0 ? m / r : vec2(0.0);
    float s = sin(th);
    return vec3(s * dir.x, cos(th), s * dir.y);
}

// The ground of height `h` at map point `m` in the land's frame (place.h's
// mapToLand: its y the height less how far the body falls away there).
vec3 landPoint(vec2 m, float h) {
    if (!landRound()) return vec3(m.x, h, m.y);
    // ((R + h) sin θ along m, not R + h: a float of R is half a metre.)
    float R = landRadius();
    float r = length(m);
    float th = r / R;
    vec2 dir = r > 0.0 ? m / r : vec2(0.0);
    float s = sin(th), hs = sin(0.5 * th);
    float sinc = th > 0.0 ? s / th : 1.0;
    vec2 xz = m * sinc + (h * s) * dir;
    return vec3(xz.x, h * cos(th) - 2.0 * R * hs * hs, xz.y);
}

// The map point under land's-frame point `p` (place.h's landToMap).
vec2 landMapOf(vec3 p) {
    if (!landRound()) return p.xz;
    float R = landRadius();
    float rho = length(p.xz);
    return rho > 0.0 ? p.xz * (R * atan(rho, R + p.y) / rho) : vec2(0.0);
}

// The ring cell wanted at map point `m` (mesh_land.vert). From the frame's
// eye, landEye, in every pass: a probe's or a turf tile's view, and the
// grass standing on the land, read the land the frame does. (No finer than
// the finest ring drawn.)
float landCell(vec2 m) {
    vec2 d = abs(m - landEye());
    return max(landCell0() * exp2(landFirstRing()), max(d.x, d.y) * landInvK());
}

// The level whose texel is half the cell wanted (a level-0 texel is half
// the finest cell, LandNode::tileConfig).
float landLevel(vec2 m) { return log2(landCell(m) / landCell0()); }

// --- the chart ---

// A round body's cube face `f`: its axes in the land's frame and where the
// anchor lies on it.
vec3 landFaceN(int f) { return LAND_W(LD_FACE / 4 + f * (LD_FACE_WORDS / 4)).xyz; }
vec3 landFaceU(int f) { return LAND_W(LD_FACE / 4 + f * (LD_FACE_WORDS / 4) + 1).xyz; }
vec3 landFaceV(int f) { return LAND_W(LD_FACE / 4 + f * (LD_FACE_WORDS / 4) + 2).xyz; }
vec4 landFaceAnchor(int f) { return LAND_W(LD_FACE / 4 + f * (LD_FACE_WORDS / 4) + 3); }

// Where a point is on the chart: its root (a cube face, 0 on the plane),
// the level-0 tile it is in and its fraction of it. (Whole tiles in an int
// and the rest apart, as a float in face metres is half a metre coarse at
// the edge of the Earth's.)
struct LandPos {
    int root;
    ivec2 cell;
    vec2 frac;
};

// The cube face body direction `d` (the land's frame) is most on.
int landFaceOf(vec3 d) {
    int f = 0;
    float best = -2.0;
    for (int i = 0; i < 6; ++i) {
        float c = dot(d, landFaceN(i));
        if (c > best) {
            best = c;
            f = i;
        }
    }
    return f;
}

// A round body's map point `m`: the face it lies on, and in `off` where it
// is on that face's chart from the anchor's point there (face metres), or,
// on a face round the body from the anchor (false), where it is from the
// face's middle (far, and coarse there).
bool landChartOff(vec2 m, out int f, out vec2 off) {
    float R = landRadius();
    float r = length(m);
    float th = r / R;
    vec2 dir = r > 0.0 ? m / r : vec2(1.0, 0.0);
    float s = sin(th), hs = sin(0.5 * th);
    vec3 d = vec3(s * dir.x, cos(th), s * dir.y);
    // d less the anchor's direction (0, 1, 0), kept small near it.
    vec3 dd = vec3(s * dir.x, -2.0 * hs * hs, s * dir.y);
    f = landFaceOf(d);
    vec3 n = landFaceN(f), u = landFaceU(f), v = landFaceV(f);
    float an = n.y;   // the anchor's d.n
    float dn = dot(d, n);
    if (an > 0.1) {
        // x = R atan(d.u / d.n): the anchor's x plus R atan of the tangent
        // of the difference, whose numerator is the difference's own.
        float ddn = dot(dd, n);
        off = R * vec2(atan(dot(dd, u) * an - u.y * ddn, dn * an + dot(d, u) * u.y),
                       atan(dot(dd, v) * an - v.y * ddn, dn * an + dot(d, v) * v.y));
        return true;
    }
    off = R * vec2(atan(dot(d, u), dn), atan(dot(d, v), dn));
    return false;
}

// landChartOff's inverse: the map point at `off` on face `f`'s chart, from
// the anchor's point there when `near`, else from the face's middle. (Near,
// as a difference from the anchor's tangents, whose own error then only
// scales the difference.)
vec2 landChartMap(int f, vec2 off, bool near) {
    float R = landRadius();
    vec3 u = landFaceU(f), v = landFaceV(f);
    vec3 w;
    if (near) {
        vec4 a = landFaceAnchor(f);
        vec2 ta = tan((a.xy + a.zw) * (LAND_W(LD_TILES / 4).x / R));
        vec2 td = tan(off / R);
        // n + t u + t' v is the anchor's |w| up plus tan(A + D) - tan A
        // along each axis.
        vec2 dt = td * (1.0 + ta * ta) / (1.0 - ta * td);
        w = vec3(0.0, sqrt(1.0 + dot(ta, ta)), 0.0) + dt.x * u + dt.y * v;
    } else {
        vec2 t = tan(off / R);
        w = landFaceN(f) + t.x * u + t.y * v;
    }
    float rho = length(w.xz);
    return rho > 0.0 ? w.xz * (R * atan(rho, w.y) / rho) : vec2(0.0);
}

LandPos landPosOf(vec2 m) {
    LandPos q;
    float tile0 = LAND_W(LD_TILES / 4).x;
    vec2 t;
    q.root = 0;
    if (!landRound()) {
        t = m / tile0;
        q.cell = ivec2(floor(t));
        q.frac = t - floor(t);
        return q;
    }
    int f;
    vec2 off;
    bool near = landChartOff(m, f, off);
    q.root = f;
    if (near) {
        vec4 a = landFaceAnchor(f);
        t = a.zw + off / tile0;
        vec2 w = floor(t);
        q.cell = ivec2(a.xy) + ivec2(w);
        q.frac = t - w;
    } else {
        t = off / tile0;
        q.cell = ivec2(floor(t));
        q.frac = t - floor(t);
    }
    return q;
}

// The ground's normal at map point `m` (on chart point `q`) whose slope is
// `g` (dh/dx, dh/dz in the chart's metres) in the land's frame.
vec3 landNormal(vec2 m, LandPos q, vec2 g) {
    if (!landRound()) return normalize(vec3(-g.x, 1.0, -g.y));
    vec3 d = landUp(m);
    vec3 n = landFaceN(q.root), u = landFaceU(q.root), v = landFaceV(q.root);
    float dn = dot(d, n);
    // A chart metre along x moves d by (I - d d^T) u sec^2(x / R) / (R |w|),
    // w = d / d.n (so |w| = 1 / d.n, and tan(x / R) = d.u / d.n): the ground
    // that much along, rising by the slope.
    float tu = dot(d, u) / dn, tv = dot(d, v) / dn;
    vec3 px = (u - dot(d, u) * d) * ((1.0 + tu * tu) * dn) + g.x * d;
    vec3 pz = (v - dot(d, v) * d) * ((1.0 + tv * tv) * dn) + g.y * d;
    // (Half the faces' u, n, v are left-handed: their cross is inward.)
    return normalize(cross(pz, px)) * sign(dot(cross(v, u), n));
}

// --- the tiles ---

// The slot of the tile of `level` holding `q`, and `uv` there; -1 when no
// tile there is resident.
int landSlot(LandPos q, int level, out vec2 uv) {
    vec4 tiles = LAND_W(LD_TILES / 4), page = LAND_W(LD_PAGE / 4);
    ivec2 ti = q.cell >> level;
    vec2 t = (vec2(q.cell - (ti << level)) + q.frac) / exp2(float(level));
    int n = int(page.y);
    uint e = texelFetch(landPage, ivec3(ti & (n - 1), q.root * int(page.z) + level), 0).r;
    uv = vec2(0.0);
    if (e == 0u) return -1;
    uvec2 hi = uvec2(ti >> int(page.x)) & 1023u;
    if (((e >> 12u) & 1023u) != hi.x || (e >> 22u) != hi.y) return -1;
    uv = (tiles.z + t * tiles.y) / (tiles.y + 2.0 * tiles.z);
    return int(e & 4095u) - 1;
}

// Where the land at `q` is read for level `lambda` (fractional: blended
// toward the next coarser across the level), or the finest coarser level
// resident when that one is not: the slots and uvs of the two levels and
// the blend (s1 < 0: one level). False when no level is (before the first
// fill).
bool landTexels(LandPos q, float lambda, out int s0, out vec2 uv0, out int s1, out vec2 uv1, out float f,
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
        s0 = landSlot(q, level, uv0);
        if (s0 >= 0) break;
    }
    if (s0 < 0) return false;
    f = lambda - float(l0);
    if (level == l0 && f > 0.0 && l0 + 1 < levels) s1 = landSlot(q, l0 + 1, uv1);
    return true;
}
bool landTexels(vec2 m, float lambda, out int s0, out vec2 uv0, out int s1, out vec2 uv1, out float f) {
    int level;
    return landTexels(landPosOf(m), lambda, s0, uv0, s1, uv1, f, level);
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


// The land at map point `m` from the tiles of level `lambda` (landTexels):
// its height and its surface (rgb its albedo, a its herbs). False when no
// level is resident.
bool landSample(vec2 m, float lambda, out float height, out vec4 surface) {
    height = 0.0;
    surface = vec4(0.18, 0.18, 0.18, 0.0);
    int s0, s1;
    vec2 uv0, uv1;
    float f;
    if (!landTexels(m, lambda, s0, uv0, s1, uv1, f)) return false;
    height = textureLod(landHeights, vec3(uv0, float(s0)), 0.0).r;
    surface = textureLod(landSurfaces, vec3(uv0, float(s0)), 0.0);
    if (s1 >= 0) {
        height = mix(height, textureLod(landHeights, vec3(uv1, float(s1)), 0.0).r, f);
        surface = mix(surface, textureLod(landSurfaces, vec3(uv1, float(s1)), 0.0), f);
    }
    return true;
}

// The land at map point `m` from the tiles of level `lambda` (landTexels)
// with its normal in the land's frame (from the heights a texel of the
// level read either way, landSlope, as the fill's) and its surface. Level
// ground where no level is resident.
bool landSampleNormal(vec2 m, float lambda, out float height, out vec3 normal, out vec4 surface) {
    height = 0.0;
    normal = landUp(m);
    surface = vec4(0.18, 0.18, 0.18, 0.0);
    LandPos q = landPosOf(m);
    int s0, s1, level;
    vec2 uv0, uv1, g;
    float f;
    if (!landTexels(q, lambda, s0, uv0, s1, uv1, f, level)) return false;
    height = landSlope(s0, uv0, level, g);
    surface = textureLod(landSurfaces, vec3(uv0, float(s0)), 0.0);
    if (s1 >= 0) {
        vec2 g1;
        height = mix(height, landSlope(s1, uv1, level + 1, g1), f);
        g = mix(g, g1, f);
        surface = mix(surface, textureLod(landSurfaces, vec3(uv1, float(s1)), 0.0), f);
    }
    normal = landNormal(m, q, g);
    return true;
}

#ifdef LAND_COVERS
// All of the land at map point `m` (landSampleNormal's height, normal and
// surface) with what covers it (LandSurface::cover, after the fill's
// clamps): x the sward's density, y its dryness, z its height multiplier,
// w the canopy. Bare level ground where no level is resident.
bool landSampleCover(vec2 m, float lambda, out float height, out vec3 normal, out vec4 surface, out vec4 cover) {
    height = 0.0;
    normal = landUp(m);
    surface = vec4(0.18, 0.18, 0.18, 0.0);
    cover = vec4(0.0, 0.0, 1.0, 0.0);
    LandPos q = landPosOf(m);
    int s0, s1, level;
    vec2 uv0, uv1, g;
    float f;
    if (!landTexels(q, lambda, s0, uv0, s1, uv1, f, level)) return false;
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
    normal = landNormal(m, q, g);
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

// The canopy at map point `m` from the tiles of level `lambda`: rgb its
// leaves' albedo, a its cover; `top` its top over the ground. None where no
// level is resident.
vec4 landSampleCanopy(vec2 m, float lambda, out float top) {
    top = 0.0;
    int s0, s1;
    vec2 uv0, uv1;
    float f;
    if (!landTexels(m, lambda, s0, uv0, s1, uv1, f)) return vec4(0.0);
    vec4 c = textureLod(landCanopies, vec3(uv0, float(s0)), 0.0);
    top = textureLod(landHeights, vec3(uv0, float(s0)), 0.0).g;
    if (s1 >= 0) {
        c = mix(c, textureLod(landCanopies, vec3(uv1, float(s1)), 0.0), f);
        top = mix(top, textureLod(landHeights, vec3(uv1, float(s1)), 0.0).g, f);
    }
    return c;
}
#endif

// The land's height at map point `m`, read at its level.
float landHeightAt(vec2 m) {
    float h;
    vec4 s;
    landSample(m, landLevel(m), h, s);
    return h;
}
