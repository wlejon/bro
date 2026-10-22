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
// alone (scene_land.glsl's landCell), never from the ring it belongs to, so two rings meeting
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

// (Of the packed vertex, only these: the caster's vertex input has no more.)
layout(location = 0) in vec3 inPos;      // cells from the ring's centre
layout(location = 2) in vec2 inUV;       // x the ring, y the morph parity

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

// Past the plants' range the land is their canopy (world/plant_canopy.h):
// it rises by their top as far as they cover it, and its fragments are
// their leaves, coming in over the range's last tenth as the plants go.
// What it rises by at `xz` (read at `lambda`, its ground `h`), and the
// canopy there (rgb its leaves' albedo, a how much of it is shown; white
// and none where it is not). The caster does not rise: the shade of a
// canopy past the plants' cast range is the land's sun's (land_sun.comp),
// as a low sun runs along its top and the shadow pages' depth offsets
// would swallow it.
float landRise(vec2 xz, float lambda, float h, out vec4 canopy) {
    canopy = vec4(1.0, 1.0, 1.0, 0.0);
#ifdef LAND_CASTER
    return 0.0;
#else
    if (!landHasCanopy()) return 0.0;
    float shown = landCanopyShown(length(vec3(xz.x, h, xz.y) - landEye3()), landCanopyRange());
    if (shown <= 0.0) return 0.0;
    float top;
    vec4 c = landSampleCanopy(xz, lambda, top);
    canopy = vec4(c.rgb, c.a * shown);
    return top * canopy.a;
#endif
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
#ifdef LAND_CASTER
    landSample(xz, lambda, h, surface);
#else
    // The normal from the heights a texel of the level read either way.
    vec3 N;
    landSampleNormal(xz, lambda, h, N, surface);
#endif
    vec4 canopy;
    h += landRise(xz, lambda, h, canopy);

    // The morph over the ring's outer cells (of the land as it rises).
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
            vec2 q = landClamp(xz + o);
            float hq = landHeightAt(q);
            vec4 cq;
            hc += hq + landRise(q, landLevel(q), hq, cq);
        }
        h = mix(h, hc / float(n), a);
    }

#ifdef LAND_CASTER
    gl_Position = push.lightViewProj * (casterModel() * vec4(xz.x, h, xz.y, 1.0));
#else
    vec3 T = normalize(vec3(1.0, 0.0, 0.0) - N * N.x);

    vec3 rel = vec3(xz.x - eye.x, h - eye.y, xz.y - eye.z);
    outWorldPos = rel;
    gl_Position = camera.viewProj * vec4(rel, 1.0);
    outNormal = N;
    outTangent = T;
    outBitangent = cross(N, T);
    outUV = xz;
    outColor = vec4(surface.rgb, 1.0);
    outInstColor = canopy;
#endif
}
