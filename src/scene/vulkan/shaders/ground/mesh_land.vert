#version 450
// The land's rings (scene/nodes/land_node.h): concentric squares of fixed
// topology, ring l's cells c_l = c_0 * 2^l, parked on the eye; this stage
// places each vertex on the land from its tiles (scene_land.glsl) and hands
// mesh.frag (or, under turf, its grass node's ground stage) what mesh.vert
// would: the camera-relative position, the normal, and the land's albedo as
// the vertex colour.
//
// CRACK-FREE BY CONSTRUCTION. A vertex at world xz is drawn at
// landSurface(xz): the tiles read at a level from xz's distance to the eye
// alone (landCell), never from the ring it belongs to, so two rings meeting
// at a boundary evaluate the same function at the same point. The level is
// continuous in distance and the tiles' levels are blended across it, so
// nothing pops as the eye moves.
//
// The rings' cell at a distance d (Chebyshev, since the rings are squares
// lined up with the axes): ring l spans N/2 cells either way of its centre
// and its hole N/4, so asking for a cell of d * K with K = 4 / N gives ring
// l's own cell at its inner edge and the next ring's at its outer: each
// ring is read at between one and two of its cells.
//
// THE MORPH. Between their vertices the rasteriser interpolates, and two
// rings with different spacing interpolate the same function differently,
// so where they overlap (the hole is inset two cells, land_node.cpp) they
// would draw two surfaces. Over a ring's outer cells each vertex moves to
// what the next coarser ring interpolates there: a vertex off that ring's
// grid (odd index; the parity rides in uv.y) to the mean of its neighbours
// a cell either way. By the overlap the two coincide everywhere.

layout(location = 0) in vec3 inPos;      // cells from the ring's centre
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec2 inUV;       // x the ring, y the morph parity
layout(location = 3) in vec4 inColor;
layout(location = 4) in vec4 inTangent;

#ifndef LAND_CASTER
layout(location = 0) out vec3 outWorldPos;
layout(location = 1) out vec3 outNormal;
layout(location = 2) out vec2 outUV;
layout(location = 3) out vec4 outColor;
layout(location = 4) out vec3 outTangent;
layout(location = 5) out vec3 outBitangent;
layout(location = 6) out vec4 outInstColor;
#endif

#include "common/scene_camera.glsl"
#include "ground/scene_land.glsl"
#ifdef LAND_CASTER
// Built again as the land's shadow caster (ShadowLandVert): the same
// surface, into a shadow view, through the caster's push (its model the
// draw's, relative to the eye the view is).
#include "common/scene_shadow_push.glsl"
#endif

const float LAND_MORPH_FULL = 7.0;   // cells from the outer edge by which the morph is complete
const float LAND_MORPH_RAMP = 9.0;   // cells it ramps over before that

// The ring cell wanted at `xz`. (From the frame's eye, landEye, in every
// pass: a probe's or a turf tile's view reads the land the frame does.)
float landCell(vec2 xz) {
    vec2 d = abs(xz - landEye());
    return max(landCell0(), max(d.x, d.y) * landInvK());
}

// The level whose texel is half the cell wanted (a level-0 texel is half
// the finest cell, LandNode::tileConfig).
float landLevel(vec2 xz) { return log2(landCell(xz) / landCell0()); }

vec2 landClamp(vec2 xz) { return clamp(xz, vec2(-landExtent()), vec2(landExtent())); }

float landHeightAt(vec2 xz) {
    float h;
    vec4 s;
    landSample(xz, landLevel(xz), h, s);
    return h;
}

void main() {
    float ring = inUV.x;
    float cl = landCell0() * exp2(ring);
    // Each ring snaps its centre to its own grid, twice its cell (so a
    // ring's grid is the next coarser one's too): a vertex stays over the
    // same ground until its ring jumps a whole step.
    // (LandNode::blockBounds places the blocks the same way.)
    float snap = 2.0 * cl;
    vec2 centre = floor(landEye() / snap) * snap;
    vec3 eye = camera.eyeWorld.xyz;
    vec2 xz = landClamp(centre + inPos.xz * cl);

    float lambda = landLevel(xz);
    float h;
    vec4 surface;
    landSample(xz, lambda, h, surface);

    // The morph over the ring's outer cells.
    float g = max(abs(inPos.x), abs(inPos.z));
    float a = clamp((g - (landRingHalf() - LAND_MORPH_FULL - LAND_MORPH_RAMP)) / LAND_MORPH_RAMP, 0.0, 1.0);
    float parity = inUV.y;
    if (a > 0.0 && parity > 0.5) {
        int n = parity < 2.5 ? 2 : 4;
        float hc = 0.0;
        for (int i = 0; i < n; ++i) {
            vec2 o;
            if (n == 2) {
                float s = i == 0 ? -cl : cl;
                o = parity < 1.5 ? vec2(s, 0.0) : vec2(0.0, s);
            } else {
                o = vec2((i & 1) == 0 ? -cl : cl, i < 2 ? -cl : cl);
            }
            hc += landHeightAt(landClamp(xz + o));
        }
        h = mix(h, hc / float(n), a);
    }

#ifdef LAND_CASTER
    gl_Position = push.lightViewProj * (casterModel() * vec4(xz.x, h, xz.y, 1.0));
#else
    // The normal from the heights half a cell either way (a texel of the
    // level read).
    float e = 0.5 * landCell(xz);
    float hx0 = landHeightAt(landClamp(xz - vec2(e, 0.0))), hx1 = landHeightAt(landClamp(xz + vec2(e, 0.0)));
    float hz0 = landHeightAt(landClamp(xz - vec2(0.0, e))), hz1 = landHeightAt(landClamp(xz + vec2(0.0, e)));
    vec3 N = normalize(vec3(hx0 - hx1, 2.0 * e, hz0 - hz1));
    vec3 T = normalize(vec3(1.0, 0.0, 0.0) - N * N.x);

    vec3 rel = vec3(xz.x - eye.x, h - eye.y, xz.y - eye.z);
    outWorldPos = rel;
    gl_Position = camera.viewProj * vec4(rel, 1.0);
    outNormal = N;
    outTangent = T;
    outBitangent = cross(N, T);
    outUV = xz;
    outColor = vec4(surface.rgb, 1.0);
    outInstColor = vec4(1.0);
#endif
}
