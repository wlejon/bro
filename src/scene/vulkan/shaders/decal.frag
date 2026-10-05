#version 450

// The camera depth policy (scene/vulkan/scene_vk_depth.h).
layout(constant_id = 0) const bool REVERSED_Z = true;

#include "scene_camera.glsl"
#include "scene_lighting.glsl"

layout(set = 2, binding = 0) uniform sampler2D uSceneDepth;
layout(set = 2, binding = 1) uniform sampler2D uAlbedoTex;
layout(set = 2, binding = 2) uniform sampler2D uEmissionTex;

layout(push_constant) uniform DecalPush {
    mat4 model;
    mat4 invModel;
    vec4 modulate;
    vec4 decalUp;      // xyz = up, w = emissionStrength
    vec4 fades;        // x: upperFade, y: lowerFade, z: normalFade, w: unused
    ivec4 flags;       // x: hasAlbedo, y: hasEmission, z: multisampled depth
} push;

layout(location = 0) out vec4 fragColor;

void main() {
    vec2 screenUV = gl_FragCoord.xy / camera.viewport.xy;
    float d = texture(uSceneDepth, screenUV).r;

    // Vulkan NDC: y = -1 is the top row, as screenUV.y = 0 is.
    vec4 ndc = vec4(screenUV * 2.0 - 1.0, d, 1.0);
    vec4 viewPos = camera.invProj * ndc;
    viewPos /= viewPos.w;
    vec3 worldPos = (camera.invView * viewPos).xyz;   // camera-relative

    // Surface normal from screen-space derivatives, taken before any discard
    // so every 2x2 quad still has all four lanes. With Vulkan's y-down window,
    // cross(dFdy, dFdx) points out of the surface (towards the viewer).
    vec3 nrm = normalize(cross(dFdy(worldPos), dFdx(worldPos)));

    // Cleared (sky) depth: nothing to project onto.
    if (REVERSED_Z ? d <= 0.0 : d >= 1.0) discard;

    // How far the reconstructed point can sit off the real surface. Under
    // MSAA the snapshot holds the NEAREST of the pixel's samples (the depth
    // resolve, scene_vk_depth.h), the depth at that sample's position rather
    // than at the pixel centre this reconstructs along: the point lands up to
    // ~0.7 px (in world units at its depth) off the surface, toward the eye.
    // A surface lying on a face of the box — a wall flush with its side —
    // would then fall just outside and lose the decal outright, so the test
    // allows that much. Single-sampled depth is the centre's own and needs
    // room only for rounding.
    float pixelWorld = 2.0 / (abs(camera.proj[1][1]) * camera.viewport.y);
    if (camera.proj[2][3] != 0.0) pixelWorld *= -viewPos.z;   // perspective: grows with depth
    float slack = pixelWorld * (push.flags.z != 0 ? 0.75 : 1e-3);
    mat3 toLocal = mat3(push.invModel);
    vec3 localPerWorld = vec3(length(vec3(toLocal[0][0], toLocal[1][0], toLocal[2][0])),
                              length(vec3(toLocal[0][1], toLocal[1][1], toLocal[2][1])),
                              length(vec3(toLocal[0][2], toLocal[1][2], toLocal[2][2])));
    vec3 local = (push.invModel * vec4(worldPos, 1.0)).xyz;
    if (any(greaterThan(abs(local), vec3(0.5) + slack * localPerWorld))) discard;
    local = clamp(local, vec3(-0.5), vec3(0.5));

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
