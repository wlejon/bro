#version 330 core
// Shadow caster shader — depth-only. Renders every shadow-casting MeshNode
// into one tile of the shadow atlas per shadow-casting light. The CPU
// pre-bakes uMVP = lightProj * lightView * meshWorldModel; no other material
// state matters because the FBO writes only depth (see shadow.frag).
//
// Compiled in up to four flavours: static, with the SKINNED macro injected
// right after the version line (withSkinnedDefine) for SkinnedMeshNode
// casters — so shadows deform with the mesh instead of staying in bind pose —
// and each of those with a user vertex chunk spliced at the marker line
// below (withUserChunk + CUSTOM_VERTEX) so vertex-displaced meshes cast the
// DISPLACED silhouette. The palette layout matches mesh.vert. The version
// directive must stay on line 1: NVIDIA's pre-scan trips over directive-like
// strings inside comments — and the splice marker must never be quoted in a
// comment either (withUserChunk replaces the FIRST occurrence).

layout(location = 0) in vec3 aPos;

#ifdef SKINNED
layout(location = 5) in uvec4 aJoints;
layout(location = 6) in vec4  aWeights;
layout(std140) uniform BonePalette {
    mat4 uBones[256];
};
#endif

#ifdef CUSTOM_VERTEX
// Mirror mesh.vert's attribute/uniform surface so a chunk that compiles
// against the mesh shader also compiles here (unused ones optimize out).
// The shadow pass uploads real wind values and applies the same pre-hook
// wind sway as mesh.vert, so the hook receives IDENTICAL object-space
// positions in both passes (a position-dependent displacement stays
// attached to its swaying mesh).
layout(location = 1) in vec3 aNormal;
layout(location = 2) in vec2 aUV;
layout(location = 3) in vec4 aColor;
layout(location = 4) in vec4 aTangent;
uniform mat4  uModel;
uniform vec3  uWindDir;
uniform float uWindStrength;
uniform float uWindTime;
uniform float uWindFreq;
uniform float uWindMask;
#endif

// Custom-shader splice point (see mesh.vert). Inert comment when unused.
//__USER_CHUNK__

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
#ifdef CUSTOM_VERTEX
    // Same hook contract as mesh.vert: object-space position post-skinning,
    // post-wind (identical wind math so the hook input matches the color
    // pass exactly). Normal/UV are passed through for the signature; only
    // the displaced position affects the depth this pass writes.
    if (uWindStrength > 0.0 && uWindMask > 0.0) {
        vec4 worldPos = uModel * vec4(lPos, 1.0);
        float bend  = aColor.r * uWindMask;
        float phase = sin(uWindTime * uWindFreq
                          + dot(worldPos.xz, vec2(0.3, 0.5)));
        vec3 deltaWorld = uWindDir * (phase * uWindStrength * bend);
        lPos += transpose(mat3(uModel)) * deltaWorld;
    }
    vec3 lNormal = aNormal;
#ifdef SKINNED
    lNormal = mat3(skinM) * aNormal;
#endif
    vec2 uv = aUV;
    userVertex(lPos, lNormal, uv);
#endif
    gl_Position = uMVP * vec4(lPos, 1.0);
}
