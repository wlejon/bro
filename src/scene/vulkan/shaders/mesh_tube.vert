#version 450
// Branch tubes: tapered capless tube walls synthesised from the vertex index
// and the node's segment records (scene_procedural.glsl) — no vertex or
// instance streams — under the node's world matrix. shadow_tube.vert is the
// caster. With vegetation records the bark bends in the wind field
// (scene_vegetation.glsl), each ring with its point on the skeleton.

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
    vec3 pos, radial, axisDir;
    vec2 uv;
    int seg;
    float endT;
    outVegShade = vec4(0.0, 0.0, 0.0, 1.0);
    if (!tubeVertex(gl_VertexIndex, pos, radial, axisDir, uv, seg, endT)) {
        gl_Position = vec4(2.0, 2.0, 2.0, 1.0);
        return;
    }
    vec4 worldPos = push.model * vec4(pos, 1.0);
    outInstColor = vec4(1.0);
    mat3 M3 = mat3(push.model);
    vec3 N = normalize(M3 * radial);
    vec3 T = normalize(M3 * axisDir);
    if (proceduralHasVegetation()) {
        vec4 v0, v1, v2, v3;
        proceduralVegetation(seg, endT, pos, v0, v1, v2, v3);
        mat3 rot;
        outVegShade = vegetationSwayAxis(push.model, v0, v1, v2, v3, vec3(0.0), N, worldPos.xyz, rot);
        outInstColor.rgb = vegetationTint(push.model, v0, v1, v2, v3, false);
        N = rot * N;
        T = rot * T;
    }
    outWorldPos = worldPos.xyz;
    gl_Position = camera.viewProj * worldPos;

    outNormal = N;
    outTangent = T;
    outBitangent = cross(outNormal, outTangent);
    outUV = uv;
    outColor = vec4(1.0);
}
