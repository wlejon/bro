#version 450
// Forest sprites (scene_forest.glsl), the far tiers: cards facing the eye on
// which mesh.frag (SCENE_FOREST_SPRITE, scene_forest_mass.glsl) places the
// leaves of a sphere's shell, lights them as the leaves tier lights its
// leaves and writes their depth.
//   mode 0 (push.extra.z), CLUMPS: each instance an entry, a run of its
//          tree's crown clumps, each drawn as the three lobes its leaves
//          fill (procClumpLobe), so the far crown keeps the near crown's
//          masses, outline, gaps and leaves;
//   mode 1, CROWNS: one card a tree over the whole crown, entry
//          gl_VertexIndex / 6 (the draw is not instanced);
//   mode 2, CORES: the leaves tier's lobes' solid cores (forestLobeCore),
//          entries as the clumps', drawn before the leaves so the leaves
//          they hide fail the depth test.
// Every sprite is posed by vegPose at its centre (a clump with its weight on
// its limb, a crown by its trunk), so it moves as the leaves it stands in
// for, and carries the gust at its tree (outColor.y) for mesh.frag's sheen:
// a gust turns leaves to show their paler undersides, so gusts roll across
// a distant canopy as lighter bands. shadow_forest_sprite.vert is the caster.
//
// The card stands at the sphere's front, facing the eye from its centre, so
// every depth mesh.frag writes lies behind it (its conservative depth test).

layout(location = 0) out vec3 outWorldPos;
layout(location = 1) out vec3 outNormal;      // toward the eye from the centre (unit)
layout(location = 2) out vec2 outUV;          // the card point, in sphere radii from the centre
layout(location = 3) out vec4 outColor;       // seed, gust, mode + 0.4 x clumps' flatness, -
layout(location = 4) out vec3 outTangent;     // the card's right (unit)
layout(location = 5) out vec3 outBitangent;   // the card's up (unit)
layout(location = 6) out vec4 outInstColor;   // tint, fade
layout(location = 7) out vec4 outVegShade;
layout(location = 8) flat out vec4 outMass0;  // centre (posed, camera-relative), radius (world)
layout(location = 9) flat out vec4 outMass1;  // the crown's centre (posed), radius
layout(location = 10) flat out vec4 outMass2; // cell, leaf length (both in radii), leaf aspect, shell
layout(location = 11) flat out vec4 outMass3; // up bias, tilt jitter, height over width, leaf length (world)
layout(location = 12) flat out vec4 outMass4; // the pose's offset, the tree (uint bits): a crown's clumps (scene_forest_crown.glsl)

#include "scene_camera.glsl"
#include "scene_mesh_push.glsl"
#include "scene_procedural.glsl"
#include "scene_vegetation.glsl"
#include "scene_forest.glsl"
#include "scene_forest_sprite_vertex.glsl"
#include "scene_occlusion.glsl"

void main() {
    // The draw's occlusion slot (occlusion_list_layout.h; -1: none).
    int slot = int(push.extra.w / 65536.0) - 1;
    int instance = gl_InstanceIndex, vertex = gl_VertexIndex;
    if (camera.occlusion.y > 1.5) {
        // The retest: one card a instance, the ones PassHiZ's test kept.
        uvec2 card = sceneOcclusionRetestCard(gl_InstanceIndex);
        instance = int(card.x);
        vertex = int(card.y) + gl_VertexIndex;
    }
    ForestSprite s;
    if (!forestSprite(int(push.extra.z + 0.5), instance, vertex, push.model, s)) {
        gl_Position = FOREST_CULLED;
        outVegShade = vec4(0.0, 0.0, 0.0, -1.0);
        return;
    }
    float D = length(s.center);
    vec3 f = D > 1e-5 ? s.center / -D : vec3(0.0, 0.0, 1.0);
    vec3 right = cross(vec3(0.0, 1.0, 0.0), f);
    right = dot(right, right) < 1e-6 ? camera.invView[0].xyz : normalize(right);
    vec3 up = cross(f, right);
    // An upright ellipsoid (a crown) seen from the side is taller or flatter
    // than it is wide; from above it is round.
    float ry = mix(s.radiusUp, s.radius, abs(f.y));
    float yScale = ry / max(s.radius, 1e-6);
    // The leaves reach past the sphere by about half a leaf.
    float margin = 1.0 + 0.6 * s.leafLen / max(s.radius, 1e-6);
    vec2 q = s.corner * margin;
    // Never nearer the eye than the eye's own near plane.
    vec3 front = s.center + f * min(s.radius, max(D - 0.05, 0.0));
    vec3 pos = front + right * (q.x * s.radius) + up * (q.y * s.radius * yScale);

    // Two-phase occlusion (scene_occlusion.glsl): the opaque batches hold
    // back a card the last frame's depth hid, and the retest draws it if
    // this frame's does not; a card behind all of a frame's depth would fail
    // the depth test at every fragment. Each of its vertices takes the same
    // four corners, so they agree, and its first lists it.
    if (camera.occlusion.x > 0.5 && camera.occlusion.y < 1.5) {
        vec3 c[8];
        for (int k = 0; k < 4; ++k) {
            vec2 qk = vec2((k & 1) != 0 ? 1.0 : -1.0, (k & 2) != 0 ? 1.0 : -1.0) * margin;
            c[k] = front + right * (qk.x * s.radius) + up * (qk.y * s.radius * yScale);
        }
        int corner = vertex % FOREST_CARD_VERTICES;
        if (sceneOcclusionHoldsBack(c, 4, camera.viewport.xy, slot, uint(instance), uint(vertex - corner),
                                    corner == 0)) {
            gl_Position = FOREST_CULLED;
            outVegShade = vec4(0.0, 0.0, 0.0, -1.0);
            return;
        }
    }

    outWorldPos = pos;
    gl_Position = camera.viewProj * vec4(pos, 1.0);
    outNormal = f;
    outTangent = right;
    outBitangent = up;
    outUV = q;
    // z: the mode, plus 0.4 times its clumps' height over width (mesh.frag
    // takes it back as the fraction).
    outColor = vec4(s.seed, s.gust, push.extra.z + 0.4 * clamp(s.lightFlat, 0.05, 1.0), 1.0);
    outInstColor = vec4(s.tint, s.fade);
    outVegShade = vec4(0.0, 0.0, 0.0, -1.0);

    // The leaves' cells: the near half of the shell holds half the leaves,
    // seen over the disc (pi in radii squared).
    float len = s.leafLen / max(s.radius, 1e-6);
    float cell = sqrt(6.2831853 / s.leaves);
    outMass0 = vec4(s.center, s.radius);
    outMass1 = s.crown;
    outMass2 = vec4(cell, len, FOREST_HEADER(FOREST_H_LEAF_ASPECT), FOREST_HEADER(FOREST_H_SHELL));
    outMass3 = vec4(FOREST_HEADER(FOREST_H_UP_BIAS), FOREST_HEADER(FOREST_H_TILT_JITTER), yScale, s.leafLen);
    outMass4 = vec4(s.offset, uintBitsToFloat(uint(s.instance)));
    // A core: its height over its width, its lobe's radius, the lobe's
    // leaves' optical depth a unit length and a leaf's length (world).
    if (push.extra.z > 1.5) outMass2 = vec4(s.core.y, s.core.x, s.core.z, s.core.w);
}
