#version 330 core
// Shadow caster shader — depth-only. Renders every shadow-casting MeshNode
// into one tile of the shadow atlas per shadow-casting light. The CPU
// pre-bakes uMVP = lightProj * lightView * meshWorldModel; no other material
// state matters because the FBO writes only depth (see shadow.frag).
//
// Compiled twice: static, and with the SKINNED macro injected right after
// the version line (withSkinnedDefine) for SkinnedMeshNode casters — so
// shadows deform with the mesh instead of staying in bind pose. The palette
// layout matches mesh.vert. The version directive must stay on line 1:
// NVIDIA's pre-scan trips over directive-like strings inside comments.

layout(location = 0) in vec3 aPos;

#ifdef SKINNED
layout(location = 5) in uvec4 aJoints;
layout(location = 6) in vec4  aWeights;
layout(std140) uniform BonePalette {
    mat4 uBones[256];
};
#endif

uniform mat4 uMVP;
void main() {
    vec3 lPos = aPos;
#ifdef SKINNED
    mat4 skinM = aWeights.x * uBones[aJoints.x]
               + aWeights.y * uBones[aJoints.y]
               + aWeights.z * uBones[aJoints.z]
               + aWeights.w * uBones[aJoints.w];
    float wSum = aWeights.x + aWeights.y + aWeights.z + aWeights.w;
    if (wSum < 0.001) skinM = mat4(1.0);
    lPos = (skinM * vec4(aPos, 1.0)).xyz;
#endif
    gl_Position = uMVP * vec4(lPos, 1.0);
}
