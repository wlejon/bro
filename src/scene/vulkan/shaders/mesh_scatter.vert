#version 450
// Foliage scatter: one instance per leaf, its transform synthesised from the
// node's branch-segment records (scene_procedural.glsl scatterLeaf) rather
// than read from an instance stream. shadow_scatter.vert is the caster. When
// the node carries vegetation records the leaf sways in the wind field
// (scene_vegetation.glsl), hanging from the point on its segment.

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

void main() {
    vec3 P, F;
    mat3 R;
    int seg;
    float along;
    if (!scatterLeaf(gl_InstanceIndex, P, R, F, seg, along)) {
        gl_Position = vec4(2.0, 2.0, 2.0, 1.0);
        outVegShade = vec4(0.0, 0.0, 0.0, 1.0);
        return;
    }

    vec4 worldPos = push.model * vec4(R * inPos + P, 1.0);
    mat3 M3 = mat3(push.model) * R;
    vec3 Nw = normalize(transpose(inverse(M3)) * inNormal);
    vec3 Tw = normalize(M3 * inTangent.xyz);
    outVegShade = vec4(0.0, 0.0, 0.0, 1.0);
    outInstColor = vec4(1.0);
    if (proceduralHasVegetation()) {
        vec4 v0, v1, v2, v3;
        proceduralVegetation(seg, along, P, F, v0, v1, v2, v3);
        mat3 rot;
        outVegShade = vegetationSway(push.model, v0, v1, v2, v3, worldPos.xyz, rot);
        outInstColor.rgb = vegetationTint(push.model, v0, v1, v2, v3);
        Nw = rot * Nw;
        Tw = rot * Tw;
    }
    outWorldPos = worldPos.xyz;
    gl_Position = camera.viewProj * worldPos;

    outNormal = Nw;
    outTangent = Tw;
    outBitangent = cross(Nw, Tw) * inTangent.w;
    outUV = inUV;
    outColor = meshVertexColorMode() != 0u ? inColor : vec4(1.0);
}
