#version 450

// The camera depth policy (scene/vulkan/scene_vk_depth.h).
layout(constant_id = 0) const bool REVERSED_Z = true;

layout(set = 0, binding = 0) uniform CameraUBO {
    mat4 view;
    mat4 proj;
    mat4 viewProj;
    mat4 invView;
    mat4 invProj;
    vec4 eyePos;
    vec4 viewport; // x: width, y: height, z: near, w: far
    vec4 fogParams;
    vec4 fogColor;
} camera;

layout(set = 1, binding = 0) uniform LightingUBO {
    vec4 sunDirection;
    vec4 sunColor;
    vec4 ambientColor;
    vec4 shadowSplits;
    mat4 shadowCascadeProj;
    vec4 numLights;
} lighting;

layout(set = 2, binding = 0) uniform sampler2D uSceneDepth;
layout(set = 2, binding = 1) uniform sampler2D uAlbedoTex;
layout(set = 2, binding = 2) uniform sampler2D uEmissionTex;

layout(push_constant) uniform DecalPush {
    mat4 model;
    mat4 invModel;
    vec4 modulate;
    vec4 decalUp;      // xyz = up, w = emissionStrength
    vec4 fades;        // x: upperFade, y: lowerFade, z: normalFade, w: unused
    ivec4 flags;       // x: hasAlbedo, y: hasEmission
} push;

layout(location = 0) out vec4 fragColor;

const float PI = 3.14159265359;

void main() {
    vec2 screenUV = gl_FragCoord.xy / camera.viewport.xy;
    float d = texture(uSceneDepth, screenUV).r;

    // Cleared (sky) depth: nothing to project onto.
    if (REVERSED_Z ? d <= 0.0 : d >= 1.0) discard;

    // In Vulkan, screenUV.y = 0 is top, 1 is bottom.
    // Vulkan NDC: x in [-1, 1], y in [-1, 1] (y=-1 is top, y=+1 is bottom).
    vec4 ndc = vec4(screenUV * 2.0 - 1.0, d, 1.0);
    vec4 pw = camera.invView * (camera.invProj * ndc);
    vec3 worldPos = pw.xyz / pw.w;

    // Surface normal from screen-space derivatives:
    // With Vulkan Y-down, cross(dFdy, dFdx) points out of the surface (towards viewer)
    vec3 nrm = normalize(cross(dFdy(worldPos), dFdx(worldPos)));

    vec3 local = (push.invModel * vec4(worldPos, 1.0)).xyz;
    if (any(greaterThan(abs(local), vec3(0.5)))) discard;

    // Projection UV: looking down -Y, U maps +X and V maps +Z
    vec2 uv = local.xz + 0.5;

    float fade = 1.0;
    if (push.fades.x > 0.0 && local.y > 0.0)
        fade *= pow(clamp(1.0 - local.y * 2.0, 0.0, 1.0), push.fades.x);
    if (push.fades.y > 0.0 && local.y < 0.0)
        fade *= pow(clamp(1.0 + local.y * 2.0, 0.0, 1.0), push.fades.y);

    float facing = dot(nrm, push.decalUp.xyz);
    if (push.fades.z > 0.0)
        fade *= smoothstep(push.fades.z, 1.0, facing * 0.5 + 0.5);

    fade *= push.modulate.a;
    if (fade <= 0.001) discard;

    vec4 albedo = (push.flags.x == 1) ? texture(uAlbedoTex, uv) : vec4(1.0);
    float a = albedo.a * fade;

    vec3 ambient = lighting.ambientColor.rgb;
    vec3 sunDir = lighting.sunDirection.xyz;
    vec3 sunCol = lighting.sunColor.rgb * lighting.sunColor.a;
    vec3 lit = ambient + sunCol * (max(dot(nrm, -sunDir), 0.0) / PI);
    vec3 rgb = albedo.rgb * push.modulate.rgb * lit * a;

    if (push.flags.y == 1) {
        float emissionStrength = push.decalUp.w;
        rgb += texture(uEmissionTex, uv).rgb * push.modulate.rgb * emissionStrength * fade;
    }

    fragColor = vec4(rgb, a);
}
