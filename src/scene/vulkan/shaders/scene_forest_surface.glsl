// The forest's placed leaves (mesh.frag's SCENE_VEGETATION builds): what a
// forest lobe, crown or lobe core (SCENE_FOREST_SPRITE) or a leaf cluster
// (SCENE_FOREST_CLUSTER) builds on its card for mesh.frag to light, as a
// ForestMass. They place their leaves here rather than drawing them
// (scene_forest_mass.glsl), discard what they do not cover and write a
// sprite's depth (gl_FragDepth, mesh.frag's REVERSED_Z). The other plants
// leave the ForestMass as forestMassNone() makes it, except the cover and
// salt mesh.frag sets for their sample mask.

struct ForestMass {
    // Placed leaves (not a drawn leaf's own face): they give the share of
    // the pixel their leaves cover, the leaves' mean normal (world, toward
    // the eye), the share showing undersides and their own lightness; a lobe
    // also where they lie.
    bool leaves;
    float cover;
    vec3 normal;
    float under, tone;
    uint salt;          // the card's dither salt (mesh.frag's forestSampleMask)
    // The sphere the leaves seen lie in (none: radius 0), and their optical
    // depth a unit length (SceneSurface::leafSphere / leafDensity).
    vec4 sphere;
    float density;
    float flatness;     // the sphere's height over its width (SceneSurface::leafFlat)
    // A sprite's only (SceneSurface's, mesh.frag sets them for SCENE_FOREST_SPRITE):
    float path;         // the light's path through the sphere's own leaves, times (leafPath)
    vec4 sun;           // SceneSurface::leafSun
    float sunN;         // SceneSurface::leafSunN
    float selfMax;      // SceneSurface::leafSelfMax
    vec3 at;            // where they lie (camera-relative): the point lit; inWorldPos unless moved
};

ForestMass forestMassNone() {
    ForestMass m;
    m.leaves = false;
    m.cover = 1.0;
    m.normal = vec3(0.0, 1.0, 0.0);
    m.under = 0.0;
    m.tone = 1.0;
    m.salt = 0u;
    m.sphere = vec4(0.0);
    m.density = 0.0;
    m.flatness = 1.0;
    m.path = 1.0;
    m.sun = vec4(0.5);
    m.sunN = 0.0;
    m.selfMax = 1e9;
    m.at = inWorldPos;
    return m;
}

#ifdef SCENE_FOREST_SPRITE
// A forest sprite (mesh_forest_sprite.vert): a lobe's core (inColor.z over
// 1.5), a crown (over 0.5) or a lobe; `flags` the mesh's (meshFlags()).
ForestMass forestSpriteSurface(uint flags) {
    ForestMass m = forestMassNone();
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
        // Every pixel of the quad on to the derivative before any leaves (a
        // miss taken at the ray's nearest approach, not a NaN): a pixel's
        // neighbours are its derivative's other half.
        float t = (-b - sqrt(max(disc, 0.0))) / a;
        m.at = d * t;
        vec3 rel = m.at - inMass0.xyz;
        // The leaves there, a leaf's length apart: each its own turn and
        // lightness, their mean once a pixel holds several.
        vec3 p = rel / max(inMass2.w, 1e-4);
        float fp = max(length(dFdx(p)), length(dFdy(p)));
        if (disc <= 0.0 || t <= 0.0) discard;
        vec3 n = normalize(vec3(rel.x, rel.y / (ky * ky), rel.z));
        vec4 g = forestMassHash(ivec2(floor(p.xz + vec2(0.37, 0.71) * floor(p.y))), uint(floor(p.y)) + 0x3c6ef372u);
        float keep = 1.0 - smoothstep(0.6, 2.0, fp);
        // Its outline ragged, leaf by leaf: the ray's nearest approach over
        // the radius, the outer fifth giving way where a leaf's draw is
        // under it (a hard disc reads as a dark spot among the sprays).
        float rim = sqrt(max(1.0 - disc / (a * inMass0.w * inMass0.w), 0.0));
        if (smoothstep(0.75, 1.0, rim) > mix(0.5, g.z, keep)) discard;
        m.leaves = true;
        m.normal = normalize(n * 0.6 + vec3(0.0, 0.4, 0.0) + (g.xyz * 2.0 - 1.0) * 0.8 * keep);
        m.tone = 0.85 + 0.3 * (g.w - 0.5) * keep;
        m.sphere = vec4(inMass0.xyz, inMass2.y);
        // As a lobe's leaves are lit (half the lobe's density, below).
        m.density = inMass2.z * 0.5;
        m.flatness = ky;
        vec4 clip = camera.viewProj * vec4(m.at, 1.0);
        gl_FragDepth = REVERSED_Z ? min(clip.z / clip.w, gl_FragCoord.z) : gl_FragCoord.z;
    } else {
        vec3 right = normalize(inTangent), up = normalize(inBitangent), view = normalize(inNormal);
        vec2 q = inUV;
        float fp = max(max(length(dFdx(q)), length(dFdy(q))), 1e-5);
        // A pixel's size (world) on the card (a crown's, at the crown).
        float pixel = max(length(dFdx(inWorldPos)), length(dFdy(inWorldPos)));
        // Past the leaves' reach (the card's corners).
        float reach = 1.0 + 0.6 * inMass2.y;
        if (dot(q, q) > reach * reach) discard;
        m.salt = uint(inColor.x * 16777216.0);
        vec3 upC = vec3(right.y, up.y, view.y);
        // How the pixel's few leaves stray from the mean (one draw serves
        // both halves: the far half's turned round, so they differ).
        vec4 grain = forestMassGrain(q, fp, inMass2.x, m.salt ^ 0x2c1b3c6du);
        float lv = max(log2(fp / inMass2.x), 0.0);
        float perPixel = (flags & MESH_FOREST_MSAA) != 0u ? clamp(exp2(2.0 * lv), 1.0, 4.0) : 1.0;
        vec4 draws = forestMassHash(ivec2(floor(q / (inMass2.x * exp2(floor(lv + 0.5))))), m.salt ^ 0x1b873593u);
        m.leaves = true;
        if (inColor.z > 0.5) {
            // A crown: its own clumps along the pixel's ray
            // (scene_forest_crown.glsl), each lit as a lobe is: through its
            // own leaves to where the light leaves it.
            vec3 d = normalize(inWorldPos);
            ForestCrownHit h = forestCrownTrace(d, int(floatBitsToUint(inMass4.w)), inMass4.xyz, inMass3.w,
                                                inMass2.z, grain, pixel, draws.x);
            m.cover = h.cover;
            if (m.cover < 1.0 / 255.0) discard;
            // The leaves face out of their clump, leaning up, each its own
            // way about that (the pixel's few, the grain).
            vec3 n = normalize(h.normal * (1.0 - inMass3.x) + vec3(0.0, inMass3.x, 0.0) +
                               (right * grain.x + up * grain.y + view * grain.z) * 0.85);
            float facing = dot(n, -d);
            m.under = smoothstep(0.1, -0.1, facing);
            m.normal = facing < 0.0 ? -n : n;
            m.tone = 1.0 + 0.1 * grain.x * grain.y;
            m.path = max(1.0 + 1.5 * grain.w, 0.05);
            m.sphere = h.sphere;
            m.density = h.density;
            m.flatness = h.flatness;
            if ((floatBitsToUint(FOREST_HEADER(FOREST_H_LOOK)) & 15u) == 0u) {
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
                m.sphere = vec4(inMass1.xyz, max(inMass1.w, length(h.at - inMass1.xyz) * 1.02));
                m.flatness = 1.0;
                m.density = FOREST_CROWN_SHADE * h.crownSigma;
                m.selfMax = FOREST_CROWN_SELF_MAX;
            } else if (lighting.sunDirection.w > 0.5) {
                // A spray crown's own leaves stop the light by the area they
                // turn to it, as a broad lobe's do (below): its sprays lean
                // up, so less of a low sun than a high one. With one depth
                // for every sun a pine's crowns at 100 / 150 / 200 m were 1.0
                // / 0.8 / 0.6 over its leaves at noon and 0.2 / 0.4 / 0.0
                // under at golden; 0.6 / 0.6 / 0.5 over and 0.1 over / 0.3
                // under / 0.0.
                float gb = clamp(1.4 * FOREST_HEADER(FOREST_H_UP_BIAS), 0.0, 1.0);
                m.density *= (1.0 - gb) + 2.0 * gb * abs(lighting.sunDirection.y);
            }
            m.at = h.at;
            vec4 clip = camera.viewProj * vec4(h.at, 1.0);
            gl_FragDepth = REVERSED_Z ? min(clip.z / clip.w, gl_FragCoord.z) : gl_FragCoord.z;
        } else {
            // The near half of the shell, and the far half through its gaps.
            ForestLeafLayer nearL = forestLobeLayerAt(q, fp, 1.0, inMass2.x, inMass2.y, inMass2.z, inMass2.w, upC,
                                                      inMass3.x, inMass3.y, m.salt, grain);
            // The far half, seen only through the near one's gaps (darker,
            // and softened by the leaves between), as its leaves' mean.
            ForestLeafLayer farL = forestLobeMean(q, -1.0, inMass2.x, inMass2.y, inMass2.z, inMass2.w, upC,
                                                  inMass3.x, grain.zwxy);
            // A spray node's lobe is its sprays hung a spray's length into
            // it from its surface, overlapping into a mass nothing shows
            // through deeper than that: within that depth's disc the far
            // half's sprays close the near half's gaps. Without it a clumps
            // tree's pads were a pale veil the ground showed through (a
            // pine at 15 / 25 m 10.7 / 8.7 levels brighter than its leaves,
            // now 3.0 / 1.3).
            bool sprays = (floatBitsToUint(FOREST_HEADER(FOREST_H_LOOK)) & 15u) != 0u;
            float coreR = sprays ? min(1.0 - inMass2.y, 1.0 - 0.5 * inMass2.w) : 0.0;
            if (coreR > 0.0 && farL.weight > 0.0)
                farL.clear *= smoothstep(0.85 * coreR, coreR, length(q));
            float wn = 1.0 - nearL.clear, wf = nearL.clear * (1.0 - farL.clear);
            m.cover = wn + wf;
            if (m.cover < 1.0 / 255.0) discard;
            float an = wn / m.cover, af = wf / m.cover;
            float kn = an / max(nearL.weight, 1e-6), kf = af / max(farL.weight, 1e-6);
            vec3 nc = nearL.normal * kn + farL.normal * kf;
            nc = dot(nc, nc) > 1e-10 ? normalize(nc) : vec3(0.0, 0.0, 1.0);
            m.normal = normalize(right * nc.x + up * nc.y + view * nc.z);
            m.under = nearL.under * kn + farL.under * kf;
            m.tone = nearL.tone * kn + farL.tone * kf;
            // Once a pixel holds several leaves, how many lie between them
            // and the light wanders with their count (sun flecks among
            // shade, the denser patches darker); a leaf drawn as itself
            // takes its sphere's mean.
            float meanShare = forestLobeMeanShare(fp, inMass2.x);
            m.path = max(1.0 + 1.5 * grain.w * meanShare, 0.05);
            // Which of them the sun reaches: the leaf seen while leaves are
            // drawn one by one, else the pixel's few (its MSAA samples'
            // leaves, as many as it holds up to four) on the grain's grid.
            // A quarter of the way from the mean to the flecks: all the way,
            // a pixel's few lit or not read as noise at this size.
            m.sun = meanShare < 0.5 ? vec4(nearL.sun) : draws;
            m.sunN = 0.25 * mix(1.0, perPixel, meanShare);
            // Lit where the leaves lie (the near half's seen, or the far
            // half's through them); the depth is the sphere's there.
            float z = sqrt(max(1.0 - dot(q, q), 0.0)) * (1.0 - 0.5 * inMass2.w);
            vec3 at = inMass0.xyz + inMass0.w * (right * q.x + up * (q.y * inMass3.z));
            m.at = at + view * (inMass0.w * (nearL.z * kn + farL.z * kf));
            vec4 clip = camera.viewProj * vec4(at + view * (inMass0.w * (an >= 0.5 ? z : -z)), 1.0);
            gl_FragDepth = REVERSED_Z ? min(clip.z / clip.w, gl_FragCoord.z) : gl_FragCoord.z;
            m.sphere = inMass0;
            // A spray node's lobe (a needle or scale-leaf form, the
            // header's look) takes no shade of its own leaves: its leaves
            // are a shell over an opaque core, and the lobes' disc casters
            // already shade it lobe by lobe as its sprays' casters do
            // theirs. With both, its clumps were 4 to 12 units darker than
            // its leaves.
            bool sprayLobe = (floatBitsToUint(FOREST_HEADER(FOREST_H_LOOK)) & 15u) != 0u;
            // 0.9 of the depth through the lobe (its path's first few shadow
            // texels skipped, SHADOW_LEAF_BLIND), times the area its leaves
            // turn to the light, as its disc caster's (they lean up, so stop
            // less of a low sun than of a high one: about (1 - b) + 2 b |L.y|,
            // b the emit's up bias). As one depth for every sun, a lobe
            // fitted to golden was too light at noon (a broadleaf's clumps
            // +1.3 to +1.8 over its leaves at 35 / 75 / 120 m) and a birch's
            // separate lobes, shaded by few discs but their own, pale balls
            // (+5 at 60 m). broflora docs/look-and-motion.md has the trade.
            float gb = clamp(1.4 * FOREST_HEADER(FOREST_H_UP_BIAS), 0.0, 1.0);
            float facing = lighting.sunDirection.w > 0.5 ? (1.0 - gb) + 2.0 * gb * abs(lighting.sunDirection.y) : 1.0;
            m.density = sprayLobe ? 0.0
                                  : forestMassDepth(inMass2.x, inMass2.y, inMass2.z) * 0.9 * facing / max(inMass0.w, 1e-3);
            // The leaves seen lie within about one optical depth of the
            // lobe's surface, as a crown's do (FOREST_CROWN_SELF_MAX): a
            // shrub's dense lobes 8.2 / 8.1 levels darker than its leaves
            // at 10 / 20 m noon, now 6.7 / 6.6; a broadleaf's unchanged.
            m.selfMax = FOREST_CROWN_SELF_MAX;
            // The lobe's height over its width, carried over its mode
            // (mesh_forest_sprite.vert).
            m.flatness = fract(inColor.z) > 1e-3 ? fract(inColor.z) / 0.4 : 1.0;
        }
    }
    return m;
}
#endif

#ifdef SCENE_FOREST_CLUSTER
ForestMass forestClusterSurface() {
    ForestMass c = forestMassNone();
    // A cluster (mesh_forest_leaf.vert): a hexagon in the kept leaf's
    // plane round its widened outline, inUV the point in leaf lengths
    // from its middle (x across, y along), inColor (widening, seed, leaf
    // width over length), inNormal / inTangent the kept leaf's normal and
    // its across direction.
    float k = inColor.x;
    vec2 p = inUV;
    float fp = max(max(length(dFdx(p)), length(dFdy(p))), 1e-5);
    c.salt = uint(inColor.y * 16777216.0);
    // The card's basis turned to the eye (z toward it), so a leaf whose
    // normal there points away shows its underside.
    vec3 n = normalize(inNormal), across = normalize(inTangent);
    bool facing = dot(n, -inWorldPos) > 0.0;
    vec3 ne = facing ? n : -n;
    vec3 along = cross(ne, across);
    vec3 lobe = vec3(dot(inLobeOut.xyz, across), dot(inLobeOut.xyz, along), dot(inLobeOut.xyz, ne));
    ForestLeafLayer L = forestClusterLayer(p, fp, k, inColor.z, facing ? 1.0 : -1.0, lobe, inLobeOut.w, c.salt);
    c.cover = 1.0 - L.clear;
    if (c.cover < 1.0 / 255.0) discard;
    vec3 m = dot(L.normal, L.normal) > 1e-10 ? normalize(L.normal) : vec3(0.0, 0.0, 1.0);
    c.normal = normalize(across * m.x + along * m.y + ne * m.z);
    // The undersides paler (as the leaves').
    c.under = L.weight > 0.0 ? L.under / L.weight : 0.0;
    c.tone = L.weight > 0.0 ? L.tone / L.weight : 1.0;
    c.leaves = true;
    // Lit as the lobe sprite that takes over from it is: through its
    // lobe's own leaves analytically, and by one shadow tap where the
    // light leaves the lobe (the casters there are the lobes' discs or
    // fewer, wider leaves, too coarse to shade a leaf among its own).
    // 0.9 of the lobe's density times its leaves' facing to the light, as
    // the lobe sprite's: a cluster stands for the leaves nearest its
    // outline (at the full depth, with the
    // path's first few shadow texels skipped as the leaves tier skips
    // them, SHADOW_LEAF_BLIND, the mid range was darker than the leaves
    // it hands over from; at half, before that skip). A broad forest's
    // leaves lie in their lobe's shell, the hollow inside crossed free
    // (forestShellPath).
    c.sphere = inLobeSphere;
    float gb = clamp(1.4 * inLobeUpBias, 0.0, 1.0);
    float lit = lighting.sunDirection.w > 0.5 ? (1.0 - gb) + 2.0 * gb * abs(lighting.sunDirection.y) : 1.0;
    c.density = inLobeDensity * 0.9 * lit;
    c.flatness = inLobeFlat;
    if (inLobeHollow > 0.0 && lighting.sunDirection.w > 0.5)
        c.density *= forestShellPath(inWorldPos, c.sphere, c.flatness, -lighting.sunDirection.xyz, inLobeHollow);
    return c;
}
#endif
