#version 450
// Forest leaf shadow casters: mesh_forest_leaf.vert's leaves into an atlas
// tile (the drawer hands casters fewer, wider leaves: a shadow texel is
// wider than a leaf). Each was posed once by forest_leaf_pose.comp's caster
// jobs (scene_forest_leaf_pose.glsl's forestCasterPoseLeaf: placed and
// swayed as its leaf); a vertex takes its point through that pose, folding
// its half of the card about the stem. The FOREST_LEAF_INLINE build (the
// casters past the pose buffer's room) poses its leaf in each vertex.

layout(location = 0) in vec3 inPos;

#include "scene_camera.glsl"
#include "scene_shadow_push.glsl"
#include "scene_procedural.glsl"
#include "scene_vegetation.glsl"
#include "scene_forest.glsl"
#ifdef FOREST_LEAF_INLINE
#include "scene_forest_shoot.glsl"
#include "scene_forest_leaf_pose.glsl"
#endif

void main() {
    ForestEntry e = forestEntry(gl_InstanceIndex);
    int leaf = e.first + gl_VertexIndex / max(e.param, 1);
#ifdef FOREST_LEAF_INLINE
    if (leaf >= e.end) {
        gl_Position = FOREST_CULLED;
        return;
    }
    forestApplyLook();
    ForestCasterPose p = forestCasterPoseLeaf(e, leaf);
#else
    if (leaf >= e.end || e.slot < 0) {
        gl_Position = FOREST_CULLED;
        return;
    }
    ForestCasterPose p = forestCasterPose(e.slot + leaf - e.first);
#endif
    if (!p.alive) {
        gl_Position = FOREST_CULLED;
        return;
    }
    // A spray's caster is its card narrowed to the share its needles or
    // scales cover (the casters have no fragment stage to cut them).
    vec3 local = inPos;
    local.x *= forestSprayFill();
    // The fold: each half of the card turned about its stem (the mesh's +z),
    // both halves rising (mesh_forest_leaf.vert's).
    float sx = local.x > 0.0 ? 1.0 : local.x < 0.0 ? -1.0 : 0.0;
    vec2 cs = sx != 0.0 ? vec2(p.fold.x, sx * p.fold.y) : vec2(1.0, 0.0);
    mat3 F = mat3(vec3(cs.x, cs.y, 0.0), vec3(-cs.y, cs.x, 0.0), vec3(0.0, 0.0, 1.0));
    vec3 pos = p.at + p.M * (F * local);
    // Set back away from the light (forestCasterPoseLeaf). (The tile's depth
    // grows away from the light.)
    if (p.setback > 0.0)
        pos += normalize(vec3(push.lightViewProj[0][2], push.lightViewProj[1][2], push.lightViewProj[2][2])) * p.setback;
    if (!forestCasterKept(pos, float(procHash(uint(leaf))) * (1.0 / 4294967296.0))) {
        gl_Position = FOREST_CULLED;
        return;
    }
    gl_Position = push.lightViewProj * vec4(pos, 1.0);
}
