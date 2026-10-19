#version 450
// Forest leaves (scene_forest.glsl): each instance is a draw entry, several
// copies of the leaf mesh end to end, copy k drawing leaf `first + k` of the
// entry's tree. Each leaf was posed once by forest_leaf_pose.comp (a broad
// leaf on its shoot, scene_forest_shoot.glsl; a spray as the clump-mode
// scatter places it; widened, swayed: scene_forest.glsl's ForestPose); a
// vertex takes its point through that pose, folding its half of the leaf
// about the stem, a broad leaf's blade out past its petiole. A tree at full
// detail has exactly the grown plant's leaves; further off the drawer keeps
// a prefix of the tree's shuffled leaves, each widened (a spray's pulled
// into its lobe, so the crown's outline holds), with the last few shrinking
// away. shadow_forest_leaf.vert is the caster.

layout(location = 0) in vec3 inPos;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec2 inUV;
layout(location = 3) in vec4 inColor;
layout(location = 4) in vec4 inTangent;

layout(location = 0) out vec3 outWorldPos;
layout(location = 1) out vec3 outNormal;
layout(location = 2) out vec2 outUV;
layout(location = 3) out vec4 outColor;
layout(location = 4) out vec3 outTangent;
layout(location = 5) out vec3 outBitangent;
layout(location = 6) out vec4 outInstColor;
layout(location = 7) out vec4 outVegShade;
// A cluster (mesh.frag's SCENE_FOREST_CLUSTER): the way the leaves round
// the kept one face before their own tilt (clumpLeafFrame's: out of the lobe
// there, leaning up; world, unit) and that tilt's size, so each leaf it
// draws turns its own way, as the leaves it stands for; and the lobe they
// lie in (centre, posed and camera-relative, and radius) with its leaves'
// optical depth a unit length, which shade them as a lobe sprite's are.
layout(location = 8) flat out vec4 outLobeOut;
layout(location = 9) flat out vec4 outLobeSphere;
layout(location = 10) flat out float outLobeDensity;
layout(location = 11) flat out float outLobeFlat;   // the lobe's height over its width
// A broad forest's lobe: its leaves lie in its shell, outside this share of
// its radius (scene_forest_mass.glsl's forestShellPath); 0 elsewhere.
layout(location = 12) flat out float outLobeHollow;

#include "scene_camera.glsl"
#include "scene_mesh_push.glsl"
#include "scene_procedural.glsl"
#include "scene_vegetation.glsl"
#include "scene_forest.glsl"
#ifdef FOREST_LEAF_INLINE
// The leaves past the pose buffer's room (the drawer's kForestLeafInline
// draws): each vertex poses its leaf itself.
#include "scene_forest_shoot.glsl"
#include "scene_forest_leaf_pose.glsl"
#endif

void main() {
    outVegShade = vec4(0.0, 0.0, 0.0, 1.0);
    outLobeOut = vec4(0.0, 1.0, 0.0, 0.0);
    outLobeSphere = vec4(0.0);
    outLobeDensity = 0.0;
    outLobeFlat = 1.0;
    outLobeHollow = 0.0;
    ForestEntry e = forestEntry(gl_InstanceIndex);
    int leaf = e.first + gl_VertexIndex / max(e.param, 1);
#ifdef FOREST_LEAF_INLINE
    if (leaf >= e.end) {
        gl_Position = FOREST_CULLED;
        return;
    }
    forestApplyLook();
    ForestPose p = forestPoseLeaf(e, leaf);
#else
    if (leaf >= e.end || e.slot < 0) {
        gl_Position = FOREST_CULLED;
        return;
    }
    int slot = e.slot + leaf - e.first;
    ForestPose p = forestPose(slot);
#endif
    if (!p.alive) {
        gl_Position = FOREST_CULLED;
        return;
    }

    bool cluster = (meshFlags() & MESH_FOREST_CLUSTER) != 0u;
    vec3 local = inPos;
    // A cut leaf (MESH_FOREST_LEAF_CUT) takes its species' leaf
    // (scene_forest.glsl's forestSpecimenLeaf): its width as a share of
    // the card's widest (`cutWidth`), and the card's three rows set out to
    // hold that outline (mesh.frag's forestLeafCut: widest a little below
    // the middle, a pointed or rounded tip), so a broad ovate leaf is not
    // clipped by a card cut for a narrower one.
    bool cut = (meshFlags() & MESH_FOREST_LEAF_CUT) != 0u;
    int specimen = cut ? forestInstanceSpecimen(e.instance) : 0;
    vec4 cutLeaf = vec4(0.0);
    float cutWidth = 1.0;
    float len = procedural.header[1].z / max(procedural.header[1].x, 1e-6);   // the mesh's own
    float halfCard = 0.5 * procedural.header[3].y * len;                      // its widest, half
    // Its petiole (forestSpecimenShoot: of the blade's length): the card's
    // stalk row stays at the node and the blade's rows move out by it, the
    // stalk cut per pixel along the first pf / (1 + pf) of the card.
    float petiole = 0.0;
    float cardT = 1.0 - inUV.y;
    if (cut) {
        cutLeaf = forestSpecimenLeaf(specimen);
        cutWidth = cutLeaf.x > 0.0 ? clamp(cutLeaf.x / max(procedural.header[3].y, 1e-3), 0.4, 1.6) : 1.0;
        float t = 1.0 - inUV.y;   // 0 the stalk's row, 0.5 the middle's, 1 the tip's
        petiole = forestLeafForm() == 0 ? clamp(forestSpecimenShoot(specimen).x, 0.0, 0.6) : 0.0;
        // A stalk under a pixel or so is not seen, and its card would only
        // cover less of the blade's (the far tiers have none): it shrinks
        // away as the leaf falls from 16 to 8 pixels long.
        if (petiole > 0.0) {
            bool ortho = abs(camera.proj[3][3]) > 0.5;
            float pxPerM = abs(camera.proj[1][1]) * camera.viewport.y * 0.5 / (ortho ? 1.0 : max(length(p.at), 1e-3));
            petiole *= smoothstep(8.0, 16.0, length(p.M[2]) * len * pxPerM);
        }
        float row = t < 0.25 ? 0.72 * clamp(1.0 - 1.2 * petiole, 0.4, 1.0)
                  : t < 0.75 ? 0.95 : mix(0.3, 0.58, clamp(cutLeaf.y, 0.0, 1.0));
        local.x = sign(inPos.x) * row * cutWidth * halfCard;
        if (t > 0.125) local.z += petiole * len;
        cardT = (t > 0.125 ? t + petiole : 0.0) / (1.0 + petiole);
    }
    // A cluster draws the leaves round the kept one, each of its own size,
    // in its plane: a hexagon round the widened outline and half a leaf more
    // (inPos the leaf's middle, inUV a corner of the unit hexagon), outUV in
    // leaf lengths. (Its pose is not widened.)
    vec2 span = vec2(0.0);
    float k = 1.0;
    if (cluster) {
        k = e.widening * clamp((e.keepEnd - float(leaf)) / max(e.tail, 1.0), 0.0, 1.0);
        float aspect = procedural.header[3].y;
        // (Its leaves join out to 0.95 of 1.27 times the outline:
        // scene_forest_mass.glsl's forestClusterLayer.)
        span = 1.1547 * vec2(0.6 * k * aspect + 0.55, 0.6 * k + 0.55);
        local += vec3(inUV.x * span.x, 0.0, inUV.y * span.y) * len;
    }

    // The fold: each half of the leaf turned about its stem (the mesh's +z),
    // both halves rising.
    float sx = local.x > 0.0 ? 1.0 : local.x < 0.0 ? -1.0 : 0.0;
    vec2 cs = sx != 0.0 ? vec2(p.fold.x, sx * p.fold.y) : vec2(1.0, 0.0);
    mat3 F = mat3(vec3(cs.x, cs.y, 0.0), vec3(-cs.y, cs.x, 0.0), vec3(0.0, 0.0, 1.0));
    mat3 M = p.M * F;
    // A leaf the eye passes through (a flight through a crown) shrinks to
    // its middle over the last metre rather than filling the view for a
    // frame or two and blinking out: at a few metres a second a leaf a few
    // centimetres off is a different picture every frame.
    float nearShrink = smoothstep(FOREST_NEAR_SHRINK.x, FOREST_NEAR_SHRINK.y, length(p.at));
    vec3 pos = p.at + M * (local * nearShrink);
    // M is a rotation times a uniform scale, so normals take it too.
    vec3 Nw = normalize(M * inNormal);
    vec3 Tw = normalize(M * inTangent.xyz);

    outVegShade = p.shade;
    outInstColor = vec4(p.tint * e.wood, e.fade);
    // A spray's (mesh.frag's SCENE_FOREST_SPRAY): its seed, its own dither.
    if (!cluster) outLobeDensity = p.seed;
    if (cluster) {
#ifndef FOREST_LEAF_INLINE
        forestPoseLobe(slot, p);
#endif
        outLobeOut = vec4(p.lobeOut, procedural.header[0].z);
        outLobeSphere = vec4(p.at + p.lobeSphere.xyz, p.lobeSphere.w);
        outLobeDensity = p.lobeDensity;
        outLobeFlat = p.lobeFlat;
        // Its shoots lie at 1 - shell * (0.2 .. 0.8) of the lobe's radius
        // (scene_forest_shoot.glsl's forestSpray), its leaves out from them.
        if (forestLeafForm() == 0) outLobeHollow = 1.0 - 0.8 * clamp(procedural.header[0].w, 0.0, 1.0);
    }

    outWorldPos = pos;
    gl_Position = camera.viewProj * vec4(pos, 1.0);
    outNormal = Nw;
    outTangent = Tw;
    outBitangent = cross(Nw, Tw) * inTangent.w;
    outUV = inUV;
    outColor = meshVertexColorMode() != 0u ? inColor : vec4(1.0);
    // A cut leaf: across its card from where the point lies (0.5 the
    // midrib, 0 and 1 the card's widest at its species' width 1), so the
    // cut runs true over each triangle.
    if (cut) {
        outUV.x = 0.5 * local.x / max(halfCard, 1e-6) + 0.5;
        outUV.y = 1.0 - cardT;
        // For mesh.frag to cut and shade: its width, its tip, its teeth a
        // side plus their depth (under 1), its petiole in 64ths of the
        // blade plus its gloss (under 1).
        outColor = vec4(cutWidth, cutLeaf.y, floor(cutLeaf.z + 0.5) + min(cutLeaf.w, 0.95),
                        floor(petiole * 64.0 + 0.5) + min(forestSpecimenGloss(specimen), 0.99));
    }
    // A cluster's point in leaf lengths, the widening, a seed and the leaf's
    // width over its length.
    if (cluster) {
        outUV = inUV * span;
        outColor = vec4(k, p.seed, procedural.header[3].y, 1.0);
    }
}
