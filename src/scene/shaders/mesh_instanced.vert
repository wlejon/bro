// Instanced mesh vertex shader. Reads the per-instance 4x3 model transform
// + RGBA tint from instance attributes (locations 8..11) and emits the same
// varyings as mesh.vert (camera-relative world position, world-space
// normal+tangent frame, UV, vertex color, distance to camera) plus an extra
// vInstColor that the instanced fragment shader multiplies into baseColor.
// uCameraEye lets the VS bake camera-relative positions on the GPU rather
// than forcing the CPU to rebuild every instance row each frame.
// The instanced fragment shader is derived from mesh.frag at runtime by
// makeMeshInstancedFragSrc() in scene_renderer_instanced.cpp.

#version 330 core
layout(location = 0) in vec3 aPos;
layout(location = 1) in vec3 aNormal;
layout(location = 2) in vec2 aUV;
layout(location = 3) in vec4 aColor;
layout(location = 4) in vec4 aTangent;
layout(location = 8)  in vec4 aInstRow0;
layout(location = 9)  in vec4 aInstRow1;
layout(location = 10) in vec4 aInstRow2;
layout(location = 11) in vec4 aInstColor;

uniform mat4 uVP;          // projection * view (no translation; camera-relative)
uniform vec3 uCameraEye;   // world-space eye, subtracted from instance translation
uniform mat4 uInstModel;   // node's parent-chain world transform (e.g. TileWorld origin)
uniform int  uUseVertexColor;

out vec3 vWorldPos;
out vec3 vNormal;
out vec2 vUV;
out vec4 vColor;
out float vCamDist;
out vec3 vTangentW;
out vec3 vBitangentW;
out vec4 vInstColor;

// Custom-shader splice point. When an instanced mesh has a user shader, the
// renderer replaces this marker line with the user's GLSL chunk (which must
// define `void userVertex(inout vec3 pos, inout vec3 normal, inout vec2 uv)`)
// and injects `#define CUSTOM_VERTEX 1` after the #version line. With no
// user chunk the marker is an inert comment and the source is unchanged.
//__USER_CHUNK__

void main() {
    // 4x3 row-major affine. Last column of each row holds translation.
    mat3 R = mat3(
        vec3(aInstRow0.x, aInstRow1.x, aInstRow2.x),
        vec3(aInstRow0.y, aInstRow1.y, aInstRow2.y),
        vec3(aInstRow0.z, aInstRow1.z, aInstRow2.z)
    );
    vec3 trans = vec3(aInstRow0.w, aInstRow1.w, aInstRow2.w);
    vec3 lPos    = aPos;
    vec3 lNormal = aNormal;
    vec2 uv      = aUV;
#ifdef CUSTOM_VERTEX
    // Custom-shader hook: runs in mesh-local space, BEFORE the per-instance
    // transform, so a displacement applies identically to every instance in
    // its own local frame (the analog of "object space" on a static mesh).
    userVertex(lPos, lNormal, uv);
#endif
    // Instance rows are node-local (relative to this InstancedMeshNode); apply
    // its parent-chain world transform (e.g. TileWorld's origin) before the
    // camera-relative offset, exactly as renderMeshNode does via uModel.
    vec3 worldPos = (uInstModel * vec4(R * lPos + trans, 1.0)).xyz;
    vec3 camRel = worldPos - uCameraEye;
    mat3 normalMat = mat3(uInstModel) * R;

    vWorldPos = camRel;
    vNormal = normalMat * lNormal;
    vTangentW   = normalMat * aTangent.xyz;
    vBitangentW = cross(vNormal, vTangentW) * aTangent.w;
    vUV = uv;
    vColor = (uUseVertexColor == 1) ? aColor : vec4(1.0);
    vCamDist = length(camRel);
    vInstColor = aInstColor;
    gl_Position = uVP * vec4(camRel, 1.0);
}
