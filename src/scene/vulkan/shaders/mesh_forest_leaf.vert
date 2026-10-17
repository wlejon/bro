#version 450
// Forest leaves (scene_forest.glsl): each instance is a draw entry, several
// copies of the leaf mesh end to end, copy k drawing leaf `first + k` of the
// entry's tree. A leaf is placed as the clump-mode scatter places it
// (clumpLeafFrame), so a tree at full detail has exactly the grown plant's
// leaves; further off the drawer keeps a prefix of the tree's shuffled
// leaves, each widened (and pulled into its lobe, so the crown's outline
// holds), with the last few shrinking away. shadow_forest_leaf.vert is the
// caster.

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

#include "scene_camera.glsl"
#include "scene_mesh_push.glsl"
#include "scene_procedural.glsl"
#include "scene_vegetation.glsl"
#include "scene_forest.glsl"

void main() {
    outVegShade = vec4(0.0, 0.0, 0.0, 1.0);
    outLobeOut = vec4(0.0, 1.0, 0.0, 0.0);
    outLobeSphere = vec4(0.0);
    outLobeDensity = 0.0;
    outLobeFlat = 1.0;
    forestApplyLook();
    ForestEntry e = forestEntry(gl_InstanceIndex);
    int leaf = e.first + gl_VertexIndex / max(e.param, 1);
    if (leaf >= e.end) {
        gl_Position = FOREST_CULLED;
        return;
    }
    // Widened, and shrinking away over the tail of the kept run.
    float k = e.widening * clamp((e.keepEnd - float(leaf)) / max(e.tail, 1.0), 0.0, 1.0);
    ivec2 lf = forestLeaf(leaf);
    vec3 P, F, lc;
    mat3 R;
    float lr;
    forestApplyClumpFlat(e.instance);
    if (k <= 0.0 || procClumpBare(e.instance, lf.x) ||
        !clumpLeafFrame(procedural.records[forestClumpRecord(lf.x)], lf.y, lf.x, P, R, F, lc, lr)) {
        gl_Position = FOREST_CULLED;
        return;
    }
    int specimen;
    float treeSeed;
    mat4 model = push.model * forestInstance(e.instance, specimen, treeSeed);

    // A widened card stands in for several leaves: it moves in toward its
    // lobe's centre by the length it grew, so it stays inside the crown.
    // Its light (depth in the crown, occlusion, tint) stays the leaf's own,
    // from where it hangs. A CLUSTER (mesh.frag's SCENE_FOREST_CLUSTER)
    // stays where the kept leaf hangs: its leaves keep to the outline the
    // leaves they stand for fill, so it reaches no further than they do.
    // It draws those leaves, each of its own size, in its plane: a hexagon
    // round the widened outline and half a leaf more (inPos the leaf's
    // middle, inUV a corner of the unit hexagon), outUV in leaf lengths.
    bool cluster = (meshFlags() & MESH_FOREST_CLUSTER) != 0u;
    vec3 rest = P;
    float grown = (k - 1.0) * procedural.header[1].z;
    if (grown > 0.0 && !cluster) P = lc + (P - lc) * clamp(1.0 - 0.5 * grown / max(lr, 1e-3), 0.55, 1.0);
    vec3 local = inPos;
    vec2 span = vec2(0.0);
    if (cluster) {
        float aspect = procedural.header[3].y;
        span = 1.1547 * vec2(0.5 * k * aspect + 0.55, 0.5 * k + 0.55);
        float len = procedural.header[1].z / max(procedural.header[1].x, 1e-6);   // the mesh's own
        local += vec3(inUV.x * span.x, 0.0, inUV.y * span.y) * len;
    } else {
        R *= k;
    }
    mat3 M3 = mat3(model) * R;
    vec3 pos = (model * vec4(R * local + P, 1.0)).xyz;
    // R and the instance are rotations times a uniform scale, so normals
    // take the same matrix.
    vec3 Nw = normalize(M3 * inNormal);
    vec3 Tw = normalize(M3 * inTangent.xyz);

    vec4 v0, v1, v2, v3;
    forestVegetation(forestClumpRecord(lf.x) + 2, rest, 1.0, v0, v1, v2, v3);
    mat3 rot;
    vec3 unswayed = pos;
    outVegShade = forestLeafSway(model, v0, v1, v2, v3, F, normalize(mat3(model) * R[1]), forestFlutter(e.widening),
                                 treeSeed, pos, rot);
    Nw = rot * Nw;
    Tw = rot * Tw;
    outInstColor = vec4(vegetationTint(model, v0, v1, v2, v3, true) * forestSpecimenTint(specimen, false), e.fade);
    // A spray's (mesh.frag's SCENE_FOREST_SPRAY): its seed, its own dither.
    if (!cluster) {
        float seed = fract(float(leaf) * 0.618034 + treeSeed);
        outLobeDensity = seed;
        // Each leaf its own lightness about the tree's (as a cluster's and a
        // lobe's leaves are, forestMassGrain): a sunlit side is leaves, not
        // one sheet. The mean is the tree's.
        outInstColor.rgb *= 0.85 + 0.3 * fract(seed * 7.31 + 0.137);
    }
    if (cluster) {
        vec3 outward = rest - lc;
        float upBias = procedural.header[0].y;
        vec3 B = (dot(outward, outward) > 1e-10 ? normalize(outward) : vec3(0.0, 1.0, 0.0)) * (1.0 - upBias) +
                 vec3(0.0, upBias, 0.0);
        B = mat3(model) * B;
        outLobeOut = vec4(dot(B, B) > 1e-10 ? normalize(rot * B) : vec3(0.0, 1.0, 0.0), procedural.header[0].z);
        // The lobe moves with its leaves (as a lobe sprite's sphere does),
        // flattened as clumpLeafFrame's: centred a tenth of its height up.
        float scale = vegScale(model);
        float r = max(lr * scale, 1e-3);
        outLobeSphere = vec4((model * vec4(lc + vec3(0.0, 0.1 * lr * procClumpFlat, 0.0), 1.0)).xyz + (pos - unswayed), r);
        // forestMassDepth's: its leaves (a third of the clump's), each
        // turned every way, over the disc, through the middle, a unit length.
        float leaves = max(procedural.records[forestClumpRecord(lf.x) + 1].w / 3.0, 1.0);
        float a = 0.5 * procedural.header[1].z * scale / r;
        outLobeDensity = leaves * a * a * procedural.header[3].y * 0.55 * 0.5 / r;
        outLobeFlat = 0.8 * procClumpFlat;
    }

    outWorldPos = pos;
    gl_Position = camera.viewProj * vec4(pos, 1.0);
    outNormal = Nw;
    outTangent = Tw;
    outBitangent = cross(Nw, Tw) * inTangent.w;
    outUV = inUV;
    outColor = meshVertexColorMode() != 0u ? inColor : vec4(1.0);
    // A cluster's point in leaf lengths, the widening, a seed and the leaf's
    // width over its length.
    if (cluster) {
        outUV = inUV * span;
        outColor = vec4(k, fract(float(leaf) * 0.618034 + treeSeed), procedural.header[3].y, 1.0);
    }
}
