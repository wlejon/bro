#version 450
// Every scene mesh: the material inputs (base colour, vertex colour, maps,
// the instance tint and atlas cell), then the shared lighting
// (scene_lighting.glsl), the air (aerial perspective or fog) and the tile
// shade map. Unlit meshes skip the
// lighting. inWorldPos is camera-relative (scene_camera.glsl): the eye is the
// origin. Built twice: as is, and with SCENE_INDIRECT_OUTPUT for the opaque
// pass while SSAO is on; and both again with SCENE_VEGETATION for plants
// (scene_vegetation.glsl): a leaf's normal leans toward its crown's bulk
// normal so the crown lights as one form, its underside reads paler, the
// sun glows through it from behind, and the crown's depth occludes the sky.

layout(location = 0) in vec3 inWorldPos;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec2 inUV;
layout(location = 3) in vec4 inColor;      // vertex colour, white when the mode is off
layout(location = 4) in vec3 inTangent;
layout(location = 5) in vec3 inBitangent;
layout(location = 6) in vec4 inInstColor;  // rgb tint, a atlas cell / 256 (white off instancing)
#ifdef SCENE_VEGETATION
// xyz the crown's bulk normal times its weight, w the ambient occlusion,
// negative on a leaf (vegetationSway), -(2 + ao) on a grass blade.
layout(location = 7) in vec4 inVegShade;
#endif
#ifdef SCENE_FOREST_SPRITE
// A forest sprite, a clump's lobe or a crown (mesh_forest_sprite.vert has
// what each carries; the common inputs above are re-purposed there too). The
// fragment places the shell's leaves on the card (scene_forest_mass.glsl),
// lights them and writes their depth.
layout(location = 8) flat in vec4 inMass0;
layout(location = 9) flat in vec4 inMass1;
layout(location = 10) flat in vec4 inMass2;
layout(location = 11) flat in vec4 inMass3;
layout(location = 12) flat in vec4 inMass4;
#endif
#ifdef SCENE_FOREST_BLOOM
// A forest bloom (mesh_forest_bloom.vert): its centre and radius, and its
// form, dot widening, seed and a spike's stalk share (scene_forest_bloom.glsl).
layout(location = 8) flat in vec4 inBloom0;
layout(location = 9) flat in vec4 inBloom1;
#endif
#if defined(SCENE_FOREST_SPRITE) || defined(SCENE_FOREST_BLOOM)
// The card stands at the sphere's front (a sprite's, a bloom's head), so the
// depth written only ever moves away from the eye: less under reversed-Z
// (the default), where the early depth test keeps working. Under the
// conventional mapping the card's own depth is written instead, which keeps
// the promise.
layout(constant_id = 0) const bool REVERSED_Z = true;
layout(depth_less) out float gl_FragDepth;
#endif
#ifdef SCENE_FOREST_CLUSTER
// The way the leaves round it face before their tilt, and the tilt; the lobe
// they lie in and its leaves' optical depth a unit length
// (mesh_forest_leaf.vert).
layout(location = 8) flat in vec4 inLobeOut;
layout(location = 9) flat in vec4 inLobeSphere;
layout(location = 10) flat in float inLobeDensity;
layout(location = 11) flat in float inLobeFlat;
#endif
#ifdef SCENE_FOREST_SPRAY
// The spray's seed (mesh_forest_leaf.vert): its own dither.
layout(location = 10) flat in float inSpraySeed;
#endif

layout(location = 0) out vec4 outColor;
#ifdef SCENE_INDIRECT_OUTPUT
// The indirect (ambient + probe) light in outColor, for the SSAO pass to
// darken after the opaque pass (scene/vulkan/pass_ssao.h).
layout(location = 1) out vec4 outIndirect;
#endif

#include "scene_camera.glsl"
#include "scene_lighting.glsl"
#include "scene_mesh_push.glsl"
#ifdef SCENE_VEGETATION
#define FOREST_MASS_FRAGMENT 1
#include "scene_forest_mass.glsl"
#endif
#ifdef SCENE_FOREST_SPRITE
#include "scene_procedural.glsl"
#include "scene_forest_crown.glsl"
#endif
#ifdef SCENE_FOREST_SPRAY
#include "scene_procedural.glsl"
#include "scene_forest_spray.glsl"
#endif
#ifdef SCENE_FOREST_BLOOM
#include "scene_forest_bloom.glsl"
#endif
#ifdef SCENE_BARK
#include "scene_bark.glsl"
#endif
#ifdef SCENE_GROUND
#include "scene_grass.glsl"
#include "scene_ground.glsl"
#endif

layout(set = 2, binding = 0) uniform sampler2D texAlbedo;
layout(set = 2, binding = 1) uniform sampler2D texNormal;
layout(set = 2, binding = 2) uniform sampler2D texMetallicRoughness;   // glTF: G roughness, B metallic
layout(set = 2, binding = 3) uniform sampler2D texEmissive;
layout(set = 2, binding = 4) uniform sampler2D texOcclusion;           // R

// The base colour uv: an instance's cell of the atlas grid, else the mesh uv.
// Only the base colour samples the atlas.
vec2 albedoUV() {
    float grid = push.extra.w;
    int cols = max(int(mod(grid, 256.0)), 1);
    int rows = max(int(grid / 256.0), 1);
    if (cols == 1 && rows == 1) return inUV;
    int cell = clamp(int(inInstColor.a * 256.0), 0, cols * rows - 1);
    vec2 cellSize = vec2(1.0 / float(cols), 1.0 / float(rows));
    return (vec2(float(cell % cols), float(cell / cols)) + fract(inUV)) * cellSize;
}

// Custom-shader splice point: a user chunk defining
// `void userFragment(inout vec3 baseColor, inout vec3 normal,
//                    inout float metallic, inout float roughness,
//                    inout vec3 emissive, inout float alpha)` replaces this
// marker and defines CUSTOM_FRAGMENT. It runs after every material input is
// gathered and before the lights, so standard lighting applies to its result.
//__USER_CHUNK__

#ifdef SCENE_VEGETATION
// The samples of this pixel a forest tier draws (ForestNode::fadeCode): a
// pixel dither h, each tier drawing where h is in its [lo, hi), the tiers of
// a tree splitting [0, 1) between them, so they cover each pixel once. The
// code is 2 * round(lo * 4096) + hi; 1 is solid. Under 4x MSAA each sample
// takes its own quarter-step of h, so a pixel is split between the tiers in
// quarters and resolves to a blend rather than a stipple; `coverage` (the
// share of the pixel placed leaves cover, scene_forest_mass.glsl) keeps that
// share of the samples by a second dither, salted by `salt` (the card's), so
// two cards over one pixel keep different samples and their cover adds up as
// leaves' would. Without MSAA a pixel is all or nothing (the mask is all
// bits).
int forestSampleMask(float code, float coverage, bool msaa, uint salt) {
    // Two independent white-noise values a pixel (an integer hash): a
    // gradient noise lines up into visible diagonal hatching at part fades.
    uvec2 q = uvec2(gl_FragCoord.xy);
    uint v = q.x * 1973u + q.y * 9277u + 0x9e3779b9u;
    v = (v ^ (v >> 16u)) * 0x7feb352du;
    v = (v ^ (v >> 15u)) * 0x846ca68bu;
    v ^= v >> 16u;
    float h = float(v & 0xffffu) * (1.0 / 65536.0);
    uint v2 = v ^ (salt * 0x9e3779b9u);
    v2 = (v2 ^ (v2 >> 15u)) * 0x2c1b3c6du;
    v2 ^= v2 >> 12u;
    float h2 = float(v2 >> 16u) * (1.0 / 65536.0);
    float k = floor(code * 0.5);
    float lo = k * (1.0 / 4096.0), hi = code - 2.0 * k;
    if (!msaa) return h >= lo && h < hi && h2 < coverage ? -1 : 0;
    int mask = 0;
    for (int s = 0; s < 4; ++s) {
        float hs = fract(h + 0.25 * float(s));
        float cs = fract(h2 + 0.25 * float(s));
        if (hs >= lo && hs < hi && cs < coverage) mask |= 1 << s;
    }
    return mask;
}
#endif

void main() {
    uint flags = meshFlags();
    vec3 worldPos = inWorldPos;
    bool frontFacing = gl_FrontFacing;
#ifdef SCENE_VEGETATION
    gl_SampleMask[0] = -1;
    // Leaves placed here rather than drawn (scene_forest_mass.glsl): a
    // forest lobe or crown (SCENE_FOREST_SPRITE) or a leaf cluster
    // (SCENE_FOREST_CLUSTER, the drawer's build for MESH_FOREST_CLUSTER). They give the share of the pixel their leaves
    // cover, the leaves' mean normal (world, toward the eye), the share
    // showing undersides and their own lightness; a lobe also where they lie.
    bool massLeaves = false;
    float massCover = 1.0;
    vec3 massN = vec3(0.0, 1.0, 0.0);
    float massUnder = 0.0, massTone = 1.0;
    uint massSalt = 0u;
    // The sphere the leaves seen lie in (none: radius 0), and their optical
    // depth a unit length (SceneSurface::leafSphere / leafDensity).
    vec4 massSphere = vec4(0.0);
    float massDensity = 0.0;
    float massFlat = 1.0;   // the sphere's height over its width (SceneSurface::leafFlat)
    // A broad forest leaf cut per pixel (forestLeafCut): its veins, its
    // margin's wave and its gloss.
    bool leafCut = false;
    float leafCutVein = 0.0, leafCutTilt = 0.0, leafCutGloss = 0.0;
#endif
#ifdef SCENE_FOREST_SPRITE
    float massPath = 1.0;   // the light's path through the sphere's own leaves, times
    vec4 massSun = vec4(0.5);   // SceneSurface::leafSun
    float massSunN = 0.0;
    if (inColor.z > 1.5) {
        // A lobe's CORE (forestLobeCore): the solid ellipsoid deep enough in
        // its leaves that nothing behind shows, its true depth written so the
        // leaves it hides fail the depth test. Its surface is where the
        // lobe's leaves are that deep: lit through them (the lobe's sphere
        // and density) as a leaf there would be, facing out of it.
        vec3 d = normalize(inWorldPos);
        float ky = inMass2.x;
        vec3 o = -inMass0.xyz;
        o.y /= ky;
        vec3 e = vec3(d.x, d.y / ky, d.z);
        float a = dot(e, e), b = dot(o, e), k = dot(o, o) - inMass0.w * inMass0.w;
        float disc = b * b - a * k;
        if (disc <= 0.0) discard;
        float t = (-b - sqrt(disc)) / a;
        if (t <= 0.0) discard;
        worldPos = d * t;
        vec3 rel = worldPos - inMass0.xyz;
        vec3 n = normalize(vec3(rel.x, rel.y / (ky * ky), rel.z));
        // The leaves there, a leaf's length apart: each its own turn and
        // lightness, their mean once a pixel holds several.
        vec3 p = rel / max(inMass2.w, 1e-4);
        float fp = max(length(dFdx(p)), length(dFdy(p)));
        vec4 g = forestMassHash(ivec2(floor(p.xz + vec2(0.37, 0.71) * floor(p.y))), uint(floor(p.y)) + 0x3c6ef372u);
        float keep = 1.0 - smoothstep(0.6, 2.0, fp);
        // Its outline ragged, leaf by leaf: the ray's nearest approach over
        // the radius, the outer fifth giving way where a leaf's draw is
        // under it (a hard disc reads as a dark spot among the sprays).
        float rim = sqrt(max(1.0 - disc / (a * inMass0.w * inMass0.w), 0.0));
        if (smoothstep(0.75, 1.0, rim) > mix(0.5, g.z, keep)) discard;
        massLeaves = true;
        massN = normalize(n * 0.6 + vec3(0.0, 0.4, 0.0) + (g.xyz * 2.0 - 1.0) * 0.8 * keep);
        massTone = 0.85 + 0.3 * (g.w - 0.5) * keep;
        massSphere = vec4(inMass0.xyz, inMass2.y);
        // As a lobe's leaves are lit (half the lobe's density, below).
        massDensity = inMass2.z * 0.5;
        massFlat = ky;
        vec4 clip = camera.viewProj * vec4(worldPos, 1.0);
        gl_FragDepth = REVERSED_Z ? min(clip.z / clip.w, gl_FragCoord.z) : gl_FragCoord.z;
    } else {
        vec3 right = normalize(inTangent), up = normalize(inBitangent), view = normalize(inNormal);
        vec2 q = inUV;
        float fp = max(max(length(dFdx(q)), length(dFdy(q))), 1e-5);
        // Past the leaves' reach (the card's corners).
        float reach = 1.0 + 0.6 * inMass2.y;
        if (dot(q, q) > reach * reach) discard;
        massSalt = uint(inColor.x * 16777216.0);
        vec3 upC = vec3(right.y, up.y, view.y);
        // How the pixel's few leaves stray from the mean (one draw serves
        // both halves: the far half's turned round, so they differ).
        vec4 grain = forestMassGrain(q, fp, inMass2.x, massSalt ^ 0x2c1b3c6du);
        float lv = max(log2(fp / inMass2.x), 0.0);
        float perPixel = (flags & MESH_FOREST_MSAA) != 0u ? clamp(exp2(2.0 * lv), 1.0, 4.0) : 1.0;
        vec4 draws = forestMassHash(ivec2(floor(q / (inMass2.x * exp2(floor(lv + 0.5))))), massSalt ^ 0x1b873593u);
        massLeaves = true;
        if (inColor.z > 0.5) {
            // A crown: its own clumps along the pixel's ray
            // (scene_forest_crown.glsl), each lit as a lobe is: through its
            // own leaves to where the light leaves it.
            vec3 d = normalize(inWorldPos);
            // A pixel's size (world) on the card, at the crown.
            float pixel = max(length(dFdx(inWorldPos)), length(dFdy(inWorldPos)));
            ForestCrownHit h = forestCrownTrace(d, int(floatBitsToUint(inMass4.w)), inMass4.xyz, inMass3.w,
                                                inMass2.z, grain, pixel, draws.x);
            massCover = h.cover;
            if (massCover < 1.0 / 255.0) discard;
            // The leaves face out of their clump, leaning up, each its own
            // way about that (the pixel's few, the grain).
            vec3 n = normalize(h.normal * (1.0 - inMass3.x) + vec3(0.0, inMass3.x, 0.0) +
                               (right * grain.x + up * grain.y + view * grain.z) * 0.85);
            float facing = dot(n, -d);
            massUnder = smoothstep(0.1, -0.1, facing);
            massN = facing < 0.0 ? -n : n;
            massTone = 1.0 + 0.1 * grain.x * grain.y;
            massPath = max(1.0 + 1.5 * grain.w, 0.05);
            massSphere = h.sphere;
            massDensity = h.density;
            massFlat = h.flatness;
            if ((floatBitsToUint(procedural.header[3].w) & 15u) == 0u) {
                // A broadleaf's crown is shaded through the whole crown's
                // leaves toward the light (its mean density), not its one
                // clump's: the crown's caster is one disc past its middle,
                // which leaves its near half unshaded, where the clumps
                // tier's lobes shade each other through their own casters
                // (its crowns were 12 units brighter than its clumps and
                // leaves). The shadow map is read where the light leaves the
                // crown's sphere, past that disc. (A spray node's crown, a
                // shell of sprays over dark cores, is as its leaves lit
                // through its one clump.)
                massSphere = vec4(inMass1.xyz, max(inMass1.w, length(h.at - inMass1.xyz) * 1.02));
                massFlat = 1.0;
                massDensity = FOREST_CROWN_SHADE * h.crownSigma;
            }
            worldPos = h.at;
            vec4 clip = camera.viewProj * vec4(h.at, 1.0);
            gl_FragDepth = REVERSED_Z ? min(clip.z / clip.w, gl_FragCoord.z) : gl_FragCoord.z;
        } else {
            // The near half of the shell, and the far half through its gaps.
            ForestLeafLayer nearL = forestLobeLayerAt(q, fp, 1.0, inMass2.x, inMass2.y, inMass2.z, inMass2.w, upC,
                                                      inMass3.x, inMass3.y, massSalt, grain);
            // The far half, seen only through the near one's gaps (darker,
            // and softened by the leaves between), as its leaves' mean.
            ForestLeafLayer farL = forestLobeMean(q, -1.0, inMass2.x, inMass2.y, inMass2.z, inMass2.w, upC,
                                                  inMass3.x, grain.zwxy);
            float wn = 1.0 - nearL.clear, wf = nearL.clear * (1.0 - farL.clear);
            massCover = wn + wf;
            if (massCover < 1.0 / 255.0) discard;
            float an = wn / massCover, af = wf / massCover;
            float kn = an / max(nearL.weight, 1e-6), kf = af / max(farL.weight, 1e-6);
            vec3 nc = nearL.normal * kn + farL.normal * kf;
            nc = dot(nc, nc) > 1e-10 ? normalize(nc) : vec3(0.0, 0.0, 1.0);
            massN = normalize(right * nc.x + up * nc.y + view * nc.z);
            massUnder = nearL.under * kn + farL.under * kf;
            massTone = nearL.tone * kn + farL.tone * kf;
            // Once a pixel holds several leaves, how many lie between them
            // and the light wanders with their count (sun flecks among
            // shade, the denser patches darker); a leaf drawn as itself
            // takes its sphere's mean.
            float meanShare = forestLobeMeanShare(fp, inMass2.x);
            massPath = max(1.0 + 1.5 * grain.w * meanShare, 0.05);
            // Which of them the sun reaches: the leaf seen while leaves are
            // drawn one by one, else the pixel's few (its MSAA samples'
            // leaves, as many as it holds up to four) on the grain's grid.
            // A quarter of the way from the mean to the flecks: all the way,
            // a pixel's few lit or not read as noise at this size.
            massSun = meanShare < 0.5 ? vec4(nearL.sun) : draws;
            massSunN = 0.25 * mix(1.0, perPixel, meanShare);
            // Lit where the leaves lie (the near half's seen, or the far
            // half's through them); the depth is the sphere's there.
            float z = sqrt(max(1.0 - dot(q, q), 0.0)) * (1.0 - 0.5 * inMass2.w);
            vec3 at = inMass0.xyz + inMass0.w * (right * q.x + up * (q.y * inMass3.z));
            worldPos = at + view * (inMass0.w * (nearL.z * kn + farL.z * kf));
            vec4 clip = camera.viewProj * vec4(at + view * (inMass0.w * (an >= 0.5 ? z : -z)), 1.0);
            gl_FragDepth = REVERSED_Z ? min(clip.z / clip.w, gl_FragCoord.z) : gl_FragCoord.z;
            massSphere = inMass0;
            // A spray node's lobe (a needle or scale-leaf form, the
            // header's look) takes no shade of its own leaves: its leaves
            // are a shell over an opaque core, and the lobes' disc casters
            // already shade it lobe by lobe as its sprays' casters do
            // theirs. With both, its clumps were 4 to 12 units darker than
            // its leaves.
            bool sprayLobe = (floatBitsToUint(procedural.header[3].w) & 15u) != 0u;
            massDensity = sprayLobe ? 0.0 : forestMassDepth(inMass2.x, inMass2.y, inMass2.z) * 0.5 / max(inMass0.w, 1e-3);
            // The lobe's height over its width, carried over its mode
            // (mesh_forest_sprite.vert).
            massFlat = fract(inColor.z) > 1e-3 ? fract(inColor.z) / 0.4 : 1.0;
        }
    }
#elif defined(SCENE_FOREST_CLUSTER)
    {
        // A cluster (mesh_forest_leaf.vert): a hexagon in the kept leaf's
        // plane round its widened outline, inUV the point in leaf lengths
        // from its middle (x across, y along), inColor (widening, seed, leaf
        // width over length), inNormal / inTangent the kept leaf's normal and
        // its across direction.
        float k = inColor.x;
        vec2 p = inUV;
        float fp = max(max(length(dFdx(p)), length(dFdy(p))), 1e-5);
        massSalt = uint(inColor.y * 16777216.0);
        // The card's basis turned to the eye (z toward it), so a leaf whose
        // normal there points away shows its underside.
        vec3 n = normalize(inNormal), across = normalize(inTangent);
        bool facing = dot(n, -inWorldPos) > 0.0;
        vec3 ne = facing ? n : -n;
        vec3 along = cross(ne, across);
        vec3 lobe = vec3(dot(inLobeOut.xyz, across), dot(inLobeOut.xyz, along), dot(inLobeOut.xyz, ne));
        ForestLeafLayer L = forestClusterLayer(p, fp, k, inColor.z, facing ? 1.0 : -1.0, lobe, inLobeOut.w, massSalt);
        massCover = 1.0 - L.clear;
        if (massCover < 1.0 / 255.0) discard;
        vec3 m = dot(L.normal, L.normal) > 1e-10 ? normalize(L.normal) : vec3(0.0, 0.0, 1.0);
        massN = normalize(across * m.x + along * m.y + ne * m.z);
        // The undersides paler (as the leaves').
        massUnder = L.weight > 0.0 ? L.under / L.weight : 0.0;
        massTone = L.weight > 0.0 ? L.tone / L.weight : 1.0;
        massLeaves = true;
        // Lit as the lobe sprite that takes over from it is: through its
        // lobe's own leaves analytically, and by one shadow tap where the
        // light leaves the lobe (the casters there are the lobes' discs or
        // fewer, wider leaves, too coarse to shade a leaf among its own).
        // Half the lobe's density: a cluster stands for the leaves nearest
        // its outline, and the full depth through the lobe left the mid
        // range darker than the leaves it hands over from.
        massSphere = inLobeSphere;
        massDensity = inLobeDensity * 0.5;
        massFlat = inLobeFlat;
    }
#elif defined(SCENE_FOREST_BLOOM)
    // A bloom (scene_forest_bloom.glsl): its form shaped on its card, its
    // colour the instance colour; lit through its own normal below.
    ForestBloomSurface bloom = forestBloomSurface(inWorldPos, inUV, normalize(inNormal), normalize(inTangent),
                                                  normalize(inBitangent), inBloom0, inBloom1, inInstColor.rgb);
    gl_FragDepth = gl_FragCoord.z;
    if (bloom.cover < 1.0 / 255.0) discard;
    massCover = bloom.cover;
    massSalt = uint(inBloom1.z * 16777216.0);
    worldPos = bloom.at;
    if (bloom.traced) {
        vec4 clip = camera.viewProj * vec4(bloom.at, 1.0);
        gl_FragDepth = REVERSED_Z ? min(clip.z / clip.w, gl_FragCoord.z) : gl_FragCoord.z;
    }
#endif
    float camDist = length(worldPos);
    if (push.extra.x > 0.0 && camDist < push.extra.x) discard;
#ifdef SCENE_VEGETATION
    if ((flags & MESH_FOREST) != 0u) {
#ifdef SCENE_FOREST_SPRAY
        // A conifer's spray (the drawer's build for a needle or scale-leaf
        // node's leaves): its needles cut out of the card.
        {
            int form = forestSprayForm();
            massCover = forestSprayCover(inUV, form, forestSprayCardAspect(form));
            if (massCover < 1.0 / 255.0) discard;
            // Each spray its own dither (two sprays over a pixel keep
            // different samples, so their cover adds up).
            massSalt = uint(inSpraySeed * 16777216.0);
        }
#else
        if ((flags & MESH_FOREST_LEAF_CUT) != 0u) {
            ForestLeafCut cut = forestLeafCut(inUV, inColor);
            massCover = cut.cover;
            if (massCover < 1.0 / 255.0) discard;
            leafCutGloss = cut.gloss;
            leafCutVein = cut.vein;
            leafCutTilt = cut.tilt;
            leafCut = true;
        }
#endif
        int mask = forestSampleMask(inInstColor.a, massCover, (flags & MESH_FOREST_MSAA) != 0u, massSalt);
        if (mask == 0) discard;
        gl_SampleMask[0] = mask;
    }
    bool forestSprite = (flags & MESH_FOREST_SPRITE) != 0u;
#endif

    // Base colour: the texture composes with the colour and the vertex tint
    // (white when off); without one the vertex colour replaces or tints it.
    vec3 baseColor;
    float alpha;
    uint vertexColor = meshVertexColorMode();
    // A cut leaf's vertex colour carries its species' leaf, not a colour.
    if ((flags & MESH_FOREST_LEAF_CUT) != 0u) vertexColor = 0u;
    if ((flags & MESH_ALBEDO_MAP) != 0u) {
        vec4 tex = texture(texAlbedo, albedoUV());
        baseColor = tex.rgb * push.baseColor.rgb * inColor.rgb;
        alpha = tex.a * push.baseColor.a * inColor.a;
    } else if (vertexColor == 1u) {
        baseColor = inColor.rgb;
        alpha = inColor.a;
    } else if (vertexColor == 2u) {
        baseColor = push.baseColor.rgb * inColor.rgb;
        alpha = push.baseColor.a * inColor.a;
    } else {
        baseColor = push.baseColor.rgb;
        alpha = push.baseColor.a;
    }
    baseColor *= inInstColor.rgb;
    if (push.pbrParams.z > 0.0 && alpha < push.pbrParams.z) discard;
#ifdef SCENE_VEGETATION
    // A grass blade (mesh_grass.vert) marks itself with w <= -2: w = -(2 +
    // ao + 2k), k (0..15) how far its translucency has faded with distance.
    // Translucent like a leaf, with no paler underside.
    bool vegLeaf = inVegShade.w < 0.0;
    bool vegBlade = inVegShade.w < -1.5;
    float vegBladeT = -inVegShade.w - 2.0;
    float vegFade = vegBlade ? floor(vegBladeT * 0.5) : 0.0;
    float vegAO = vegBlade ? vegBladeT - 2.0 * vegFade : abs(inVegShade.w);
    // A leaf's face is the one its normal points to. Its frame (the clump
    // and scatter placements, clumpLeafFrame, bromesh's leaf scatter) is a
    // mirror image (across, normal, tip are left-handed), so its triangles
    // wind the other way and gl_FrontFacing names the wrong face: the eye
    // would see a sunlit top lit as its own underside, and a leaf against
    // the sun lit on the side away from it.
    if (vegLeaf && !vegBlade) frontFacing = dot(inNormal, -inWorldPos) >= 0.0;
    // Placed leaves take the leaf's colour, their tree's and limb's tint
    // and their own lightness (the vertex colour carries their data).
    vec3 massBulk = vec3(0.0, 1.0, 0.0);
    if (massLeaves) {
        baseColor = push.baseColor.rgb * inInstColor.rgb * massTone;
        alpha = 1.0;
#ifdef SCENE_FOREST_SPRITE
        // The crown's depth tint and ambient occlusion at where the leaves
        // lie, as the leaves have them (vegetationTint, vegCrownShade).
        vec3 deep;
        forestCrownTerms((worldPos - inMass1.xyz) / inMass1.w, massBulk, vegAO, deep);
        baseColor *= deep;
#endif
    }
    // A leaf's underside is paler and yellower than its top.
    vec3 vegUnder = baseColor * vec3(1.25, 1.3, 1.05) + vec3(0.015, 0.02, 0.0);
    if (massLeaves) baseColor = mix(baseColor, vegUnder, 0.6 * massUnder);
    else if (vegLeaf && !vegBlade && !forestSprite && !frontFacing) baseColor = mix(baseColor, vegUnder, 0.6);
    // A cut leaf's veins are paler and yellower than the blade between.
    if (leafCut) baseColor = mix(baseColor, baseColor * vec3(1.35, 1.45, 1.05) + vec3(0.01, 0.015, 0.0), leafCutVein);
    // In a gust (inColor.y, the drag at its tree) some of a sprite's leaves
    // turn their undersides up, so the crown lightens as it passes over.
    if (forestSprite) {
        float sheen = clamp((inColor.y - 0.12) * 1.6, 0.0, 1.0);
        baseColor = mix(baseColor, baseColor * vec3(1.25, 1.3, 1.05) + vec3(0.015, 0.02, 0.0), 0.4 * sheen);
    }
#endif
#ifdef SCENE_FOREST_BLOOM
    // A bloom's colour is its form's (petals, centre, florets, stalk), and
    // a head is darker toward its underside.
    baseColor = bloom.albedo;
    alpha = 1.0;
    vegAO *= bloom.ao;
#endif

    vec3 emissive = push.emissive.rgb * push.emissive.a;
    vec3 geomN = normalize(inNormal);
    SceneAir air = sceneAir(worldPos, camDist);

    // Unlit is the authored base colour as is: no lights and no emissive
    // (GL's unlit branch), only the air in front of it.
    if ((flags & MESH_UNLIT) != 0u) {
        vec3 color = baseColor * air.transmittance + air.inscatter;
        if ((flags & MESH_SHADE_MAP) != 0u && lighting.shadeOrigin.w > 0.5) color *= cellShade(worldPos, geomN);
        // Unlit surfaces reflect nothing in the SSR mask phase.
        outColor = vec4(color, (flags & MESH_REFLECTANCE) != 0u ? 0.0 : mix(alpha, 0.0, air.fade));
#ifdef SCENE_INDIRECT_OUTPUT
        outIndirect = vec4(0.0);
#endif
        return;
    }

    // Two-sided thin surfaces shade the face the camera sees.
    vec3 N = geomN;
    if ((flags & MESH_TWO_SIDED) != 0u && !frontFacing) N = -N;
    if ((flags & (MESH_NORMAL_MAP | MESH_HAS_TANGENTS)) == (MESH_NORMAL_MAP | MESH_HAS_TANGENTS)) {
        vec3 nTS = texture(texNormal, inUV).xyz * 2.0 - 1.0;
        N = normalize(mat3(normalize(inTangent), normalize(inBitangent), N) * nTS);
    }
    vec3 shadePos = worldPos;
#ifdef SCENE_VEGETATION
    // Placed leaves are lit through their mean normal, which faces the eye.
    if (massLeaves) N = massN;
    // The crown as one form: the leaf's own normal blended toward the bulk
    // (a sprite's from where its leaves lie, the rest's from the vertex).
    if (forestSprite) {
        N = normalize(N * 0.4 + massBulk * 0.6);
    } else {
        float bulkWeight = length(inVegShade.xyz);
        float bulkKeep = 1.0;
#ifndef SCENE_FOREST_SPRAY
        if (vegLeaf && !vegBlade && !massLeaves) {
            // Near, a leaf is its own face, not the crown's: folded along
            // its midrib (the halves tilt apart, so one takes more light than
            // the other) and turned its own way more than the bulk's, so the
            // leaves of a sunlit side differ leaf by leaf rather than lying
            // as one flat sheet. The halves curve away from a midrib that
            // is level (the tilt grows as the square of the way out), so a
            // leaf shades round, not as two flat facets. Gone by the
            // distance a leaf is a few pixels.
            float near = 1.0 - smoothstep(6.0, 25.0, camDist);
            if (near > 0.0) {
                float across = clamp(inUV.x * 2.0 - 1.0, -1.0, 1.0);
                vec3 T = normalize(inTangent);
                N = normalize(N - (frontFacing ? 1.0 : -1.0) * 0.55 * near * across * abs(across) * T);
                // A spiny leaf's margin waves up and down between its spines.
                if (leafCut) N = normalize(N + near * leafCutTilt * normalize(inBitangent));
                bulkKeep = 1.0 - 0.5 * near;
            }
        }
#endif
        if (bulkWeight > 1e-4)
            N = normalize(N * (1.0 - bulkWeight * bulkKeep) + inVegShade.xyz * bulkKeep);
    }
#endif
#ifdef SCENE_FOREST_BLOOM
    N = bloom.normal;
#endif

    float metallic = push.pbrParams.x;
    float roughness = push.pbrParams.y;
    if ((flags & MESH_METALLIC_ROUGHNESS) != 0u) {
        vec4 mr = texture(texMetallicRoughness, inUV);
        roughness *= mr.g;
        metallic *= mr.b;
    }
    if ((flags & MESH_EMISSIVE_MAP) != 0u) emissive *= texture(texEmissive, inUV).rgb;
#ifdef SCENE_VEGETATION
    // A waxy leaf (a holly's) has a tight, bright highlight.
    if (leafCut) roughness = mix(roughness, 0.26, leafCutGloss);
#endif
#ifdef SCENE_BARK
    // A plant's wood (scene_bark.glsl): inColor where on the stem, inUV its
    // height over the root and the bark word (negative: not a plant's).
    if (inUV.y >= 0.0) barkSurface(inColor, inUV.x, inUV.y, worldPos, N, inTangent, baseColor, N, roughness, vegAO);
#endif
#ifdef SCENE_GROUND
    // Turf (scene_ground.glsl): the vertex colour is the ground's data
    // (density, dryness, floor) where the grass node's field does not reach.
    float groundAO = 1.0;
    groundSurface(worldPos, inColor, baseColor, roughness, groundAO);
    baseColor *= push.baseColor.rgb;
#endif

#ifdef CUSTOM_FRAGMENT
    userFragment(baseColor, N, metallic, roughness, emissive, alpha);
    N = normalize(N);
#endif

    SceneSurface s;
    s.position = shadePos;
    s.normal = N;
    s.view = normalize(-worldPos);
    s.camDist = camDist;
    s.baseColor = baseColor;
    s.metallic = clamp(metallic, 0.0, 1.0);
    s.roughness = clamp(roughness, 0.04, 1.0);
    s.receivesShadow = (flags & MESH_RECEIVES_SHADOW) != 0u;
    s.twoSided = (flags & MESH_TWO_SIDED) != 0u;
    s.subsurface = push.extra.y;
    s.translucency = 0.0;
#ifdef SCENE_VEGETATION
    if (vegLeaf) s.translucency = (push.extra.y > 0.0 ? min(1.4 * push.extra.y, 1.0) : 0.6) * (1.0 - vegFade / 15.0);
    // A waxy leaf is thick: less light comes through it.
    if (leafCut) s.translucency *= 1.0 - 0.5 * leafCutGloss;
#endif
#ifdef SCENE_FOREST_BLOOM
    // Thin petals let the light through; a head's florets and a spike less.
    s.translucency = bloom.translucency;
#endif
    s.leafSphere = vec4(0.0);
    s.leafDensity = 0.0;
    s.leafPath = 1.0;
    s.leafFlat = 1.0;
    s.leafSun = vec4(0.5);
    s.leafSunN = 0.0;
#ifdef SCENE_FOREST_SPRITE
    s.leafPath = massPath;
    s.leafSun = massSun;
    s.leafSunN = massSunN;
#endif
#ifdef SCENE_VEGETATION
    s.leafSphere = massSphere;
    s.leafDensity = massDensity;
    s.leafFlat = massFlat;
#endif

    vec3 indirect = sceneAmbient(s);
    if ((flags & MESH_OCCLUSION_MAP) != 0u) indirect *= texture(texOcclusion, inUV).r;
#ifdef SCENE_VEGETATION
    indirect *= vegAO;
#endif
#ifdef SCENE_GROUND
    indirect *= groundAO;
#endif
    vec3 color = sceneDirectLight(s) + indirect + emissive;

    // SSR mask phase: alpha carries the reflectance the SSR pass weights its
    // reflections by (normal-incidence Fresnel luminance times smoothness^2);
    // the SSR pass restores coverage before anything blends against it.
    if ((flags & MESH_REFLECTANCE) != 0u) {
        vec3 F0 = mix(vec3(0.04), s.baseColor, s.metallic);
        alpha = dot(F0, vec3(0.2126, 0.7152, 0.0722)) * (1.0 - s.roughness) * (1.0 - s.roughness);
    }

    color = color * air.transmittance + air.inscatter;
    indirect *= air.transmittance;
    alpha = mix(alpha, 0.0, air.fade);

    if ((flags & MESH_SHADE_MAP) != 0u && lighting.shadeOrigin.w > 0.5) {
        float shade = cellShade(worldPos, geomN);
        color *= shade;
        indirect *= shade;
    }

    outColor = vec4(color, alpha);
#ifdef SCENE_INDIRECT_OUTPUT
    outIndirect = vec4(indirect, 0.0);
#endif
}
