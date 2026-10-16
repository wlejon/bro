#version 450
// Forest bark (scene_forest.glsl): each instance is a draw entry, a run of
// its tree's segments (thickest first, so a run from the first is always
// connected to the root) as tapered tube walls of `param` sides, built from
// the vertex index like mesh_tube.vert, mitred at the joints, and bending in
// the wind field. shadow_forest_tube.vert is the caster.

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
#include "scene_forest_tube.glsl"

void main() {
    outVegShade = vec4(0.0, 0.0, 0.0, 1.0);
    ForestEntry e = forestEntry(gl_InstanceIndex);
    vec3 pos, radial, axisDir;
    vec2 uv;
    int seg;
    float endT;
    if (!forestTubeVertex(e, gl_VertexIndex, pos, radial, axisDir, uv, seg, endT)) {
        gl_Position = FOREST_CULLED;
        return;
    }
    int specimen;
    float treeSeed;
    mat4 model = push.model * forestInstance(e.instance, specimen, treeSeed);
    vec4 world = model * vec4(pos, 1.0);
    mat3 M3 = mat3(model);
    vec3 N = normalize(M3 * radial);
    vec3 T = normalize(M3 * axisDir);
    vec4 v0, v1, v2, v3;
    forestVegetation(forestSegmentRecord(seg) + 3, pos, endT, v0, v1, v2, v3);
    mat3 rot;
    outVegShade = vegetationSwayAxis(model, v0, v1, v2, v3, vec3(0.0), N, world.xyz, rot);
    outInstColor = vec4(vegetationTint(model, v0, v1, v2, v3, false), e.fade);
    N = rot * N;
    T = rot * T;

    outWorldPos = world.xyz;
    gl_Position = camera.viewProj * world;
    outNormal = N;
    outTangent = T;
    outBitangent = cross(N, T);
    outUV = uv;
    outColor = vec4(1.0);
}
