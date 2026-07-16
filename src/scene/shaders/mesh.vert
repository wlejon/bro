#version 330 core
// vWorldPos is in camera-relative space (uModel has eye pre-subtracted).
// That keeps precision at planet scale and means uCameraPos == 0, which
// simplifies the view-vector math in the fragment shader.
//
// Compiled twice: once as-is for static meshes and once with the SKINNED
// macro injected right after the version line (see withSkinnedDefine) for
// SkinnedMeshNode. The skinned variant blends a bone-matrix palette (final
// skinning matrices, world(bone) * inverseBind — the layout
// bromesh::computeSkinningMatrices produces) into position, normal, AND
// tangent before the model matrix, so the PBR TBN stays correct under
// deformation. NOTE: the version directive must stay on line 1 — NVIDIA's
// pre-scan even trips over directive-like strings inside comments.

layout(location = 0) in vec3 aPos;
layout(location = 1) in vec3 aNormal;
layout(location = 2) in vec2 aUV;
layout(location = 3) in vec4 aColor;
layout(location = 4) in vec4 aTangent;   // xyz = tangent, w = handedness

#ifdef SKINNED
layout(location = 5) in uvec4 aJoints;   // 4 bone indices (u16 stream)
layout(location = 6) in vec4  aWeights;  // 4 normalized weights

// 256 mat4 = 16 KB = GL 3.3 core's guaranteed minimum uniform block size.
// Cap rationale lives at SkinnedMeshNode::kMaxBones.
layout(std140) uniform BonePalette {
    mat4 uBones[256];
};
#endif

uniform mat4 uMVP;
uniform mat4 uModel;
uniform int uUseVertexColor;
uniform vec3  uWindDir;
uniform float uWindStrength;
uniform float uWindTime;
uniform float uWindFreq;
uniform float uWindMask;   // per-mesh opt-in (0 = static, 1 = sway). Multiplied
                           // into vertex-color R so flora opts in but terrain
                           // (whose color R is per-vertex shade) stays still.

out vec3 vWorldPos;
out vec3 vNormal;
out vec2 vUV;
out vec4 vColor;
out float vCamDist;
out vec3 vTangentW;
out vec3 vBitangentW;

void main() {
    vec3 lPos     = aPos;
    vec3 lNormal  = aNormal;
    vec3 lTangent = aTangent.xyz;

#ifdef SKINNED
    mat4 skinM = aWeights.x * uBones[aJoints.x]
               + aWeights.y * uBones[aJoints.y]
               + aWeights.z * uBones[aJoints.z]
               + aWeights.w * uBones[aJoints.w];
    // Unweighted vertices (weight sum ~0) stay in bind pose rather than
    // collapsing to the origin.
    float wSum = aWeights.x + aWeights.y + aWeights.z + aWeights.w;
    if (wSum < 0.001) skinM = mat4(1.0);
    lPos = (skinM * vec4(aPos, 1.0)).xyz;
    mat3 skinN = mat3(skinM);
    lNormal  = skinN * aNormal;
    lTangent = skinN * aTangent.xyz;
#endif

    vec4 worldPos = uModel * vec4(lPos, 1.0);
    mat3 M3 = mat3(uModel);
    vec3 swayedAPos = lPos;
    if (uWindStrength > 0.0 && uWindMask > 0.0) {
        float bend  = aColor.r * uWindMask;
        float phase = sin(uWindTime * uWindFreq
                          + dot(worldPos.xz, vec2(0.3, 0.5)));
        vec3 deltaWorld = uWindDir * (phase * uWindStrength * bend);
        // Push the world delta back into object space (orthonormal-rotation
        // approximation of inverse(M3)) so the same uMVP can transform it.
        vec3 deltaObj   = transpose(M3) * deltaWorld;
        swayedAPos += deltaObj;
        worldPos.xyz += deltaWorld;
    }
    vWorldPos = worldPos.xyz;
    vNormal = M3 * lNormal;
    vTangentW   = M3 * lTangent;
    vBitangentW = cross(vNormal, vTangentW) * aTangent.w;
    vUV = aUV;
    vColor = (uUseVertexColor == 1) ? aColor : vec4(1.0);
    vCamDist = length(worldPos.xyz);
    gl_Position = uMVP * vec4(swayedAPos, 1.0);
}
