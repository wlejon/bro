#version 450

layout(location = 0) in vec2 inUV;
layout(location = 0) out vec4 outColor;

layout(set = 0, binding = 0) uniform sampler2D uSharp;
layout(set = 0, binding = 1) uniform sampler2D uBlur;
layout(set = 0, binding = 2) uniform sampler2D uDepthTex;

layout(push_constant) uniform DoFPushConstants {
    float focusDistance;
    float focusRange;
    float nearPlane;
    float farPlane;
    float isPerspective;
} push;

float linearizeDepth(float d) {
    float n = push.nearPlane;
    float f = push.farPlane;
    if (push.isPerspective > 0.5) {
        return n * f / max(d * (f - n) + n, 1e-9);
    }
    return n + (1.0 - d) * (f - n);
}

void main() {
    vec4 sharp = texture(uSharp, inUV);
    vec4 blur  = texture(uBlur, inUV);
    float d = texture(uDepthTex, inUV).r;
    float dist = linearizeDepth(d);
    float coc = clamp((abs(dist - push.focusDistance) - push.focusRange)
                      / max(push.focusRange, 1e-3), 0.0, 1.0);
    coc = coc * coc * (3.0 - 2.0 * coc);
    outColor = mix(sharp, blur, coc);
}
