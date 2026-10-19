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
#include "scene_forest_shoot.glsl"

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
    bool onShoot;
    ForestLeafAt at;
    forestApplyClumpFlat(e.instance);
    if (k <= 0.0 || procClumpBare(e.instance, lf.x) || !forestLeafFrame(e.instance, lf, P, R, F, lc, lr, onShoot, at)) {
        gl_Position = FOREST_CULLED;
        return;
    }
    forestSprayBrush(e.instance, lf.x, lc, lr, P, R, F);
    // Swayed about where it hangs as its leaf is (scene_forest_leaf_pose.glsl),
    // its spray's and shoot's bending included: a leaf's own shadow on the
    // leaves under it stays with it as a gust bends its spray.
    vec3 rest = P;
    float grown = (k - 1.0) * FOREST_HEADER(FOREST_H_LEAF_LENGTH);
    // A widened broad caster grows from its node as its leaf does
    // (forestPoseLeaf).
    if (grown > 0.0 && !onShoot) P = lc + (P - lc) * clamp(1.0 - 0.5 * grown / max(lr, 1e-3), 0.55, 1.0);
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
    // light by part of the reach it gained, the crown's shadow starts about
    // where the leaves' does. (The tile's depth grows away from the light.)
    // At 0.35 of it, set back past leaves it should shade: a broadleaf's
    // thinned crown at golden 45 / 75 m 1.5 / 7.0 levels brighter than its
    // leaves; at 0.15, 0.2 / 5.5 (at none, -0.6 / 4.1).
    if (grown > 0.0) {
        vec3 away = normalize(vec3(push.lightViewProj[0][2], push.lightViewProj[1][2], push.lightViewProj[2][2]));
        pos += away * (0.15 * grown * vegScale(model));
    }
    vec4 v0, v1, v2, v3;
    forestVegetation(forestClumpRecord(lf.x) + FOREST_CLUMP_VEGETATION / 4, rest, 1.0, v0, v1, v2, v3);
    VegPlant plant = vegPlant(model, v0, v3);
    VegLimb limb = vegLimb(model, v3);
    VegWind w = vegWind(plant);
    vec3 anchor = (model * vec4(v2.xyz, 1.0)).xyz;
    VegPose pose = vegPose(plant, limb, w, anchor, clamp(v2.w, 0.0, 1.0));
    float amount = forestFlutter(e.widening);
    if (onShoot) {
        vec3 off, om;
        forestLeafShootSway(at, model, pose.drag, amount, off, om);
        pose.offset += off;
        pose.omega += om;
    }
    float seed = fract(sin(dot(v2.xyz, vec3(12.9898, 78.233, 37.719)) + treeSeed * 61.7) * 43758.5453);
    mat3 rot = vegLeafAmount(pose, w, normalize(mat3(model) * F), normalize(mat3(model) * R[1]), pos - anchor, seed,
                             amount);
    pos = anchor + pose.offset + rot * (pos - anchor);
    gl_Position = push.lightViewProj * vec4(pos, 1.0);
}
