#version 450

// Applies SSAO to the opaque surfaces' indirect light only. Drawn into the
// HDR scope with REVERSE_SUBTRACT blending, so the result is
//   colour - indirect * (1 - visibility)
// which leaves direct light, emission and everything drawn after the opaque
// pass untouched (scene/vulkan/pass_ssao.h).

layout(location = 0) in vec2 inUV;
layout(location = 0) out vec4 outColor;

layout(set = 0, binding = 0) uniform sampler2D uAoTex;        // half-res visibility, blurred
layout(set = 0, binding = 1) uniform sampler2D uIndirectTex;  // the opaque pass's indirect light

layout(push_constant) uniform PushConsts {
    float uIntensity;
} push;

void main() {
    float ao = texture(uAoTex, inUV).r;
    float visibility = clamp(mix(1.0, ao, push.uIntensity), 0.0, 1.0);
    vec3 indirect = texelFetch(uIndirectTex, ivec2(gl_FragCoord.xy), 0).rgb;
    outColor = vec4(indirect * (1.0 - visibility), 0.0);
}
