#version 450
// Foliage scatter shadow casters: mesh_scatter.vert's leaves into an atlas
// tile, swaying with them when the node carries vegetation records.

layout(location = 0) in vec3 inPos;

#include "common/scene_camera.glsl"
#include "common/scene_shadow_push.glsl"
#include "common/scene_procedural.glsl"
#include "common/scene_vegetation.glsl"

void main() {
    vec3 P, F;
    mat3 R;
    int seg;
    float along;
    int leaf;
    if (!scatterLeafIndex(gl_InstanceIndex, gl_VertexIndex, leaf) || !scatterLeaf(leaf, P, R, F, seg, along)) {
        gl_Position = vec4(2.0, 2.0, 2.0, 1.0);
        return;
    }
    mat4 model = casterModel();
    vec4 worldPos = model * vec4(R * inPos + P, 1.0);
    if (proceduralHasVegetation()) {
        vec4 v0, v1, v2, v3;
        proceduralVegetation(seg, along, P, v0, v1, v2, v3);
        mat3 rot;
        vegetationSwayAxis(model, v0, v1, v2, v3, F, normalize(mat3(model) * R[1]), worldPos.xyz, rot);
    }
    gl_Position = push.lightViewProj * worldPos;
}
