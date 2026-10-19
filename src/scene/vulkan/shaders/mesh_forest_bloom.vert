#version 450
// Forest blooms (scene_forest.glsl): each instance is a draw entry, a run of
// its tree's blooms (the entry's param is the blooms' first record), each a
// card of two triangles on which mesh.frag's SCENE_FOREST_BLOOM build
// (scene_forest_bloom.glsl) shapes the specimen's bloom form, nothing baked:
//   FLOWER (1)  a disc across the way it faces (its twig's outward direction
//               tilted up), just past the twig's tip, cut into five broad
//               petals round a yellow centre;
//   HEAD (2)    a ball of four-petalled florets set on the tip, nodding a
//               little: a card at the ball's front facing the eye, the ball
//               traced per pixel (its depth written);
//   SPIKE (3)   a bare stalk standing upright on the tip with a tapering
//               spike of florets above it: a card turning about its axis to
//               face the eye.
// A bloom is posed rigidly by its twig's sway (its segment's vegetation
// record at the segment's tip, as the bark there and the leaves on it are):
// the card moves with the pose's offset and turns with its rotation, so a
// spike leans as its stalk's shoot bends.
//
// A bloom smaller on screen than FOREST_BLOOM_DOT_PX across its slenderest
// dimension is drawn that size, a dot whose cover the fragment cuts to the
// share of it the bloom covers (the dithered sample mask), so blooms dwindle
// into the distance without popping, into the crown tier's range; one whose
// area is under FOREST_BLOOM_CULL_PX squared is not drawn at all (its cover
// would be a few hundredths). No caster: blooms cast no shadow.

layout(location = 0) out vec3 outWorldPos;
layout(location = 1) out vec3 outNormal;      // the card's facing (unit)
layout(location = 2) out vec2 outUV;          // the card point (see each form)
layout(location = 3) out vec4 outColor;       // white: blooms take no vertex colour
layout(location = 4) out vec3 outTangent;     // the card's right (unit)
layout(location = 5) out vec3 outBitangent;   // the card's up (unit; a spike's axis)
layout(location = 6) out vec4 outInstColor;   // the bloom's colour, fade
layout(location = 7) out vec4 outVegShade;    // (0, 0, 0, ambient occlusion): not a leaf
layout(location = 8) flat out vec4 outBloom0; // centre (posed, camera-relative), radius (world): a head's ball, a flower's disc
layout(location = 9) flat out vec4 outBloom1; // form, dot widening (1: drawn as itself), seed, a spike's stalk share of its card

#include "scene_camera.glsl"
#include "scene_mesh_push.glsl"
#include "scene_procedural.glsl"
#include "scene_vegetation.glsl"
#include "scene_forest.glsl"

const float FOREST_BLOOM_DOT_PX = 1.5;
const float FOREST_BLOOM_CULL_PX = 0.25;

void cull() {
    gl_Position = FOREST_CULLED;
    outVegShade = vec4(0.0, 0.0, 0.0, 1.0);
}

void main() {
    outColor = vec4(1.0);
    outVegShade = vec4(0.0, 0.0, 0.0, 1.0);
    outBloom0 = vec4(0.0);
    outBloom1 = vec4(0.0);
    forestApplyLook();
    ForestEntry e = forestEntry(gl_InstanceIndex);
    int bloom = e.first + gl_VertexIndex / FOREST_CARD_VERTICES;
    if (bloom >= e.end) { cull(); return; }
    int specimen;
    float treeSeed;
    mat4 model = push.model * forestInstance(e.instance, specimen, treeSeed);
    vec4 look = forestSpecimenBloom(specimen);
    int form = int(look.w + 0.5);
    if (form < 1 || form > 3) { cull(); return; }
    int r = e.param + bloom * FOREST_BLOOM_RECORDS;
    vec4 b0 = FOREST_RECORD(r, FOREST_BLOOM_POSITION);
    vec4 b1 = FOREST_RECORD(r, FOREST_BLOOM_DIR);
    int seg = int(floatBitsToUint(b1[FOREST_BLOOM_SEGMENT % 4]));
    float scale = vegScale(model);
    float size = b0[FOREST_BLOOM_SIZE % 4] * scale;

    // Posed as its twig's tip.
    vec4 v0, v1, v2, v3;
    forestVegetation(forestSegmentRecord(seg) + FOREST_SEG_VEGETATION / 4, b0.xyz, 1.0, v0, v1, v2, v3);
    VegPlant plant = vegPlant(model, v0, v3);
    VegLimb limb = vegLimb(model, v3);
    VegWind w = vegWind(plant);
    vec3 tip = (model * vec4(b0.xyz, 1.0)).xyz;
    VegPose pose = vegPose(plant, limb, w, tip, clamp(v2.w, 0.0, 1.0));
    mat3 rot = vegRotation(pose.omega);
    vec3 at = tip + pose.offset;
    vec3 d0 = mat3(model) * b1.xyz;
    vec3 dir = rot * (dot(d0, d0) > 1e-12 ? normalize(d0) : vec3(0.0, 1.0, 0.0));
    float seed = fract(float(bloom) * 0.618034 + treeSeed * 7.31);

    // Pixels a metre where it stands.
    float dist = max(length(at), 1e-3);
    bool ortho = abs(camera.proj[3][3]) > 0.5;
    float pxPerM = abs(camera.proj[1][1]) * camera.viewport.y * 0.5 / (ortho ? 1.0 : dist);

    const vec2 kCorners[6] = vec2[6](vec2(-1.0, -1.0), vec2(1.0, -1.0), vec2(1.0, 1.0),
                                     vec2(-1.0, -1.0), vec2(1.0, 1.0), vec2(-1.0, 1.0));
    vec2 corner = kCorners[gl_VertexIndex % FOREST_CARD_VERTICES];
    const vec3 up = vec3(0.0, 1.0, 0.0);
    vec3 pos, N, R, U;
    float g = 1.0, stalk = 0.0;
    if (form == 2) {
        // A head: a ball half its size across, nodding from the tip, set
        // most of its radius past it. Its florets stand a little proud.
        float rb = 0.5 * size;
        vec3 c = at + normalize(dir + up * 0.6) * (0.6 * rb);
        float px = size * pxPerM;
        if (px < FOREST_BLOOM_CULL_PX) { cull(); return; }
        g = max(1.0, FOREST_BLOOM_DOT_PX / px);
        float D = length(c);
        vec3 f = D > 1e-5 ? -c / D : vec3(0.0, 0.0, 1.0);
        R = cross(up, f);
        R = dot(R, R) < 1e-6 ? camera.invView[0].xyz : normalize(R);
        U = cross(f, R);
        // The card at the ball's front (never nearer than the near plane),
        // so the depth the fragment writes only moves away from the eye.
        vec3 front = c + f * min(rb, max(D - 0.05, 0.0));
        float reach = 1.08 * g;
        pos = front + (R * corner.x + U * corner.y) * (rb * reach);
        N = f;
        outUV = corner * reach;   // in ball radii
        outBloom0 = vec4(c, rb);
    } else if (form == 1) {
        // A flower: facing its twig's way tilted up, just past the tip. As a
        // dot it faces the eye.
        float rf = 0.5 * size;
        float px = size * pxPerM;
        if (px < FOREST_BLOOM_CULL_PX) { cull(); return; }
        g = max(1.0, FOREST_BLOOM_DOT_PX / px);
        vec3 F = normalize(dir + up * 0.9);
        vec3 c = at + F * (0.12 * rf);
        if (g > 1.0) F = normalize(-c);
        R = perpendicularUnit(F);
        U = cross(F, R);
        pos = c + (R * corner.x + U * corner.y) * (rf * g);
        N = F;
        outUV = corner * g;   // in flower radii
        outBloom0 = vec4(c, rf);
    } else {
        // A spike: a stalk 0.9 of its length, then the spike, held upright
        // (leaning only as its shoot bends), a card about its axis facing
        // the eye, `hw` half its width.
        float L = size, Ls = 0.9 * size, hw = 0.09 * size;
        vec3 A = normalize(rot * up);
        float px = 2.0 * hw * pxPerM;
        if (sqrt(2.0 * hw * L) * pxPerM < FOREST_BLOOM_CULL_PX) { cull(); return; }
        g = max(1.0, FOREST_BLOOM_DOT_PX / px);
        vec3 mid = at + A * (0.5 * (Ls + L));
        vec3 V = normalize(-mid);
        R = cross(A, V);
        R = dot(R, R) < 1e-8 ? camera.invView[0].xyz : normalize(R);
        N = cross(R, A);
        U = A;
        float y = corner.y * 0.5 + 0.5;
        pos = at + A * (y * (Ls + L)) + R * (corner.x * hw * g);
        outUV = vec2(corner.x * g, y);   // across in half-widths, along over the whole card
        stalk = Ls / (Ls + L);
        outBloom0 = vec4(at, hw);
    }

    outWorldPos = pos;
    gl_Position = camera.viewProj * vec4(pos, 1.0);
    outNormal = N;
    outTangent = R;
    outBitangent = U;
    // Each bloom a little lighter or darker; inside the crown a little
    // less sky (the crown's occlusion where it stands, as a leaf there).
    outInstColor = vec4(look.rgb * (0.88 + 0.24 * fract(seed * 5.71)), e.fade);
    outVegShade = vec4(0.0, 0.0, 0.0, abs(vegCrownShade(model, v1, tip).w));
    outBloom1 = vec4(float(form), g, seed, stalk);
}
