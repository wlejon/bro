#version 450
// Forest leaf shadow casters: mesh_forest_leaf.vert's leaves into an atlas
// tile, posed by the same sway (the drawer hands casters fewer, wider
// leaves: a shadow texel is wider than a leaf).

layout(location = 0) in vec3 inPos;

#include "scene_camera.glsl"
#include "scene_shadow_push.glsl"
#include "scene_procedural.glsl"
#include "scene_vegetation.glsl"
#include "scene_forest.glsl"

void main() {
    forestApplyLook();
    ForestEntry e = forestEntry(gl_InstanceIndex);
    int leaf = e.first + gl_VertexIndex / max(e.param, 1);
    if (leaf >= e.end) {
        gl_Position = FOREST_CULLED;
        return;
    }
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
    forestSprayBrush(e.instance, lf.x, lc, lr, P, R, F);
    // Swayed about where it hangs, as mesh_forest_leaf.vert sways it.
    vec3 rest = P;
    float grown = (k - 1.0) * procedural.header[1].z;
    if (grown > 0.0) P = lc + (P - lc) * clamp(1.0 - 0.5 * grown / max(lr, 1e-3), 0.55, 1.0);
    R *= k;
    // A spray's caster is its card narrowed to the share its needles or
    // scales cover (the casters have no fragment stage to cut them).
    R[0] *= forestSprayFill();

    int specimen;
    float treeSeed;
    mat4 model = casterModel() * forestInstance(e.instance, specimen, treeSeed);
    vec3 pos = (model * vec4(R * inPos + P, 1.0)).xyz;
    // A widened caster holds the area of the leaves it stands for, but
    // reaches further from its centre than they do: its shadow would fall on
    // leaves that stand above it toward the light. Set back away from the
    // light by about the reach it gained, the crown's shadow starts where
    // the leaves' does. (The tile's depth grows away from the light.)
    if (grown > 0.0) {
        vec3 away = normalize(vec3(push.lightViewProj[0][2], push.lightViewProj[1][2], push.lightViewProj[2][2]));
        pos += away * (0.35 * grown * vegScale(model));
    }
    vec4 v0, v1, v2, v3;
    forestVegetation(forestClumpRecord(lf.x) + 2, rest, 1.0, v0, v1, v2, v3);
    mat3 rot;
    forestLeafSway(model, v0, v1, v2, v3, F, normalize(mat3(model) * R[1]), forestFlutter(e.widening), treeSeed,
                   pos, rot);
    gl_Position = push.lightViewProj * vec4(pos, 1.0);
}
