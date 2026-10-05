#version 450
// Branch tubes: tapered capless tube walls synthesised from the vertex index
// and the node's segment records (scene_procedural.glsl) — no vertex or
// instance streams — under the node's world matrix. shadow_tube.vert is the
// caster.

layout(location = 0) out vec3 outWorldPos;
layout(location = 1) out vec3 outNormal;
layout(location = 2) out vec2 outUV;
layout(location = 3) out vec4 outColor;
layout(location = 4) out vec3 outTangent;
layout(location = 5) out vec3 outBitangent;
layout(location = 6) out vec4 outInstColor;

#include "scene_camera.glsl"
#include "scene_mesh_push.glsl"
#include "scene_procedural.glsl"

void main() {
    vec3 pos, radial, axisDir;
    vec2 uv;
    if (!tubeVertex(gl_VertexIndex, pos, radial, axisDir, uv)) {
        gl_Position = vec4(2.0, 2.0, 2.0, 1.0);
        return;
    }
    vec4 worldPos = push.model * vec4(pos, 1.0);
    outWorldPos = worldPos.xyz;
    gl_Position = camera.viewProj * worldPos;

    mat3 M3 = mat3(push.model);
    outNormal = normalize(M3 * radial);
    outTangent = normalize(M3 * axisDir);
    outBitangent = cross(outNormal, outTangent);
    outUV = uv;
    outColor = vec4(1.0);
    outInstColor = vec4(1.0);
}
