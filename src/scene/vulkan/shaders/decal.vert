#version 450

layout(location = 0) in vec3 inPos;

layout(set = 0, binding = 0) uniform CameraUBO {
    mat4 view;
    mat4 proj;
    mat4 viewProj;
    mat4 invView;
    mat4 invProj;
    vec4 eyePos;
    vec4 viewport;
    vec4 fogParams;
    vec4 fogColor;
} camera;

layout(push_constant) uniform DecalPush {
    mat4 model;
    mat4 invModel;
    vec4 modulate;
    vec4 decalUp;      // xyz = up, w = emissionStrength
    vec4 fades;        // x: upperFade, y: lowerFade, z: normalFade, w: unused
    ivec4 flags;       // x: hasAlbedo, y: hasEmission
} push;

void main() {
    gl_Position = camera.viewProj * (push.model * vec4(inPos, 1.0));
}
