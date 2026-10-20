#version 450
// Bloom bright pass: the HDR energy above a luminance threshold, with a soft
// knee one threshold wide so the onset is not a hard edge. Written at half
// resolution, blurred by blur.frag and added back by tonemap.frag.

layout(location = 0) in vec2 inUV;
layout(location = 0) out vec4 outColor;

layout(set = 0, binding = 0) uniform sampler2D texSource;

layout(push_constant) uniform BrightPush {
    float threshold;
} push;

void main() {
    vec3 c = texture(texSource, inUV).rgb;
    float luma = dot(c, vec3(0.2126, 0.7152, 0.0722));
    float k = clamp((luma - push.threshold) / max(push.threshold, 1e-3), 0.0, 1.0);
    outColor = vec4(c * k, 1.0);
}
