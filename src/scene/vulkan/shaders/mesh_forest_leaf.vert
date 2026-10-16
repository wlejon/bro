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

#include "scene_camera.glsl"
#include "scene_mesh_push.glsl"
#include "scene_procedural.glsl"
#include "scene_vegetation.glsl"
#include "scene_forest.glsl"

void main() {
    outVegShade = vec4(0.0, 0.0, 0.0, 1.0);
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
    if (k <= 0.0 || !clumpLeafFrame(procedural.records[forestClumpRecord(lf.x)], lf.y, lf.x, P, R, F, lc, lr)) {
        gl_Position = FOREST_CULLED;
        return;
    }
    // A widened leaf stands in for several: it moves in toward its lobe's
    // centre by the length it grew, so it stays inside the crown.
    float grown = (k - 1.0) * procedural.header[1].z;
    if (grown > 0.0) P = lc + (P - lc) * clamp(1.0 - 0.5 * grown / max(lr, 1e-3), 0.55, 1.0);
    R *= k;

    int specimen;
    float treeSeed;
    mat4 model = push.model * forestInstance(e.instance, specimen, treeSeed);
    mat3 M3 = mat3(model) * R;
    vec3 pos = (model * vec4(R * inPos + P, 1.0)).xyz;
    // R and the instance are rotations times a uniform scale, so normals
    // take the same matrix.
    vec3 Nw = normalize(M3 * inNormal);
    vec3 Tw = normalize(M3 * inTangent.xyz);

    vec4 v0, v1, v2, v3;
    forestVegetation(forestClumpRecord(lf.x) + 2, P, 1.0, v0, v1, v2, v3);
    mat3 rot;
    outVegShade = forestLeafSway(model, v0, v1, v2, v3, F, normalize(mat3(model) * R[1]), forestFlutter(e.widening),
                                 treeSeed, pos, rot);
    Nw = rot * Nw;
    Tw = rot * Tw;
    outInstColor = vec4(vegetationTint(model, v0, v1, v2, v3, true), e.fade);

    outWorldPos = pos;
    gl_Position = camera.viewProj * vec4(pos, 1.0);
    outNormal = Nw;
    outTangent = Tw;
    outBitangent = cross(Nw, Tw) * inTangent.w;
    outUV = inUV;
    outColor = meshVertexColorMode() != 0u ? inColor : vec4(1.0);
}
