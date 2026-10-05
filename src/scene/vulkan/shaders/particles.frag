#version 450

// The camera depth policy (scene/vulkan/scene_vk_depth.h).
layout(constant_id = 0) const bool REVERSED_Z = true;

layout(location = 0) in vec2 inUV;
layout(location = 1) in vec4 inColor;

layout(set = 1, binding = 0) uniform sampler2D uTex;
layout(set = 1, binding = 1) uniform sampler2D uSceneDepth;

layout(set = 0, binding = 0) uniform CameraUBO {
    mat4 view;
    mat4 proj;
    mat4 viewProj;
    mat4 invView;
    mat4 invProj;
    vec4 eyeWorld;
    vec4 viewport; // x=w, y=h, z=near, w=far
    vec4 fogParams;
    vec4 fogColor;
} camera;

layout(push_constant) uniform ParticlePush {
    mat4 model;
    vec4 camRight;
    vec4 camUp;
    vec4 params; // x: cols, y: rows, z: mode, w: softDist
} push;

layout(location = 0) out vec4 fragColor;

// Window depth -> eye distance in world units. An orthographic projection
// (proj[3][3] = 1) maps depth linearly, reversed like the perspective one.
float linearizeDepth(float d) {
    float n = camera.viewport.z;
    float f = camera.viewport.w;
    if (camera.proj[3][3] > 0.5) return n + (REVERSED_Z ? 1.0 - d : d) * (f - n);
    if (REVERSED_Z) return n * f / max(d * (f - n) + n, 1e-7);   // d = 1 at near, 0 at far
    return n * f / max(f - d * (f - n), 1e-7);                    // d = 0 at near, 1 at far
}

void main() {
    float a;
    vec3 rgb;
    int mode = int(push.params.z);
    if (mode == 1) {
        vec4 tex = texture(uTex, inUV);
        a = tex.a * inColor.a;
        rgb = tex.rgb * inColor.rgb;
    } else {
        vec2 p = inUV - 0.5;
        float d = length(p) * 2.0;
        float t = clamp(1.0 - d, 0.0, 1.0);
        a = inColor.a * t * t;
        rgb = inColor.rgb;
    }

    float softDist = push.params.w;
    if (softDist > 0.0) {
        vec2 screenUV = gl_FragCoord.xy / camera.viewport.xy;
        float sceneD = texture(uSceneDepth, screenUV).r;
        float sceneZ = linearizeDepth(sceneD);
        float fragZ  = linearizeDepth(gl_FragCoord.z);
        a *= clamp((sceneZ - fragZ) / softDist, 0.0, 1.0);
    }

    if (a <= 0.001) discard;
    fragColor = vec4(rgb * a, a);
}
