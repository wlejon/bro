#version 450

// The camera depth policy (scene/vulkan/scene_vk_depth.h).
layout(constant_id = 0) const bool REVERSED_Z = true;

layout(location = 0) in vec2 inUV;
layout(location = 0) out vec4 outColor;
// The opaque scope's indirect-light attachment, when SSAO adds one: the sky
// has no indirect light for AO to take away. Without the attachment the
// write goes nowhere.
layout(location = 1) out vec4 outIndirect;

layout(set = 0, binding = 0) uniform samplerCube texCubemap;

layout(push_constant) uniform EnvPush {
    mat4 invViewProj;
    vec4 sunDir;         // xyz = sun direction, w = hasCubemap (0 or 1)
    vec4 skyColor;       // rgb = sky color, a = intensity
    vec4 horizonColor;   // rgb = horizon color, a = sun size
    vec4 groundColor;    // rgb = ground color, a = sun intensity
} push;

void main() {
    outIndirect = vec4(0.0);
    vec2 ndc = inUV * 2.0 - 1.0;
    vec4 clipFar = vec4(ndc, REVERSED_Z ? 0.0 : 1.0, 1.0);
    vec4 worldFar = push.invViewProj * clipFar;
    float len = length(worldFar.xyz);
    vec3 rayDir = (len > 0.0001) ? (worldFar.xyz / len) : vec3(0.0, 0.0, -1.0);

    if (push.sunDir.w > 0.5) {
        outColor = texture(texCubemap, rayDir) * push.skyColor.a;
        return;
    }

    float up = rayDir.y;
    vec3 col;
    if (up > 0.0) {
        float t = pow(clamp(up, 0.0, 1.0), 0.5);
        col = mix(push.horizonColor.rgb, push.skyColor.rgb, t);
    } else {
        float t = pow(clamp(-up, 0.0, 1.0), 0.5);
        col = mix(push.horizonColor.rgb, push.groundColor.rgb, t);
    }

    vec3 sunD = normalize(push.sunDir.xyz);
    float sunDot = max(dot(rayDir, sunD), 0.0);
    float sunDisc = smoothstep(0.998, 0.9995, sunDot);
    col += vec3(1.0, 0.95, 0.8) * sunDisc * push.groundColor.a;

    outColor = vec4(col * push.skyColor.a, 1.0);
}
