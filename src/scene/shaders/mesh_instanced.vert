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

void main() {
    // 4x3 row-major affine. Last column of each row holds translation.
    mat3 R = mat3(
        vec3(aInstRow0.x, aInstRow1.x, aInstRow2.x),
        vec3(aInstRow0.y, aInstRow1.y, aInstRow2.y),
        vec3(aInstRow0.z, aInstRow1.z, aInstRow2.z)
    );
    vec3 trans = vec3(aInstRow0.w, aInstRow1.w, aInstRow2.w);
    // Instance rows are node-local (relative to this InstancedMeshNode); apply
    // its parent-chain world transform (e.g. TileWorld's origin) before the
    // camera-relative offset, exactly as renderMeshNode does via uModel.
    vec3 worldPos = (uInstModel * vec4(R * aPos + trans, 1.0)).xyz;
    vec3 camRel = worldPos - uCameraEye;
    mat3 normalMat = mat3(uInstModel) * R;

    vWorldPos = camRel;
    vNormal = normalMat * aNormal;
    vTangentW   = normalMat * aTangent.xyz;
    vBitangentW = cross(vNormal, vTangentW) * aTangent.w;
    vUV = aUV;
    vColor = (uUseVertexColor == 1) ? aColor : vec4(1.0);
    vCamDist = length(camRel);
    vInstColor = aInstColor;
    gl_Position = uVP * vec4(camRel, 1.0);
}
