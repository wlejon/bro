#version 450
// Tilt-shift composite: sharp to blurred by vertical distance from a focus
// band, then the miniature grade (saturation, contrast about mid grey). The
// band is measured from the bottom of the frame, as the GL renderer's was.
// Alpha rides through so a scene with no background stays transparent.

layout(location = 0) in vec2 inUV;
layout(location = 0) out vec4 outColor;

layout(set = 0, binding = 0) uniform sampler2D texSharp;
layout(set = 0, binding = 1) uniform sampler2D texBlur;

layout(push_constant) uniform TiltPush {
    float focusCenter;
    float focusWidth;
    float feather;
    float saturation;
    float contrast;
} push;

void main() {
    vec4 sharp = texture(texSharp, inUV);
    vec4 blur = texture(texBlur, inUV);
    float d = abs((1.0 - inUV.y) - push.focusCenter);
    float t = smoothstep(push.focusWidth, push.focusWidth + push.feather, d);
    vec3 c = mix(sharp.rgb, blur.rgb, t);
    float luma = dot(c, vec3(0.299, 0.587, 0.114));
    c = mix(vec3(luma), c, push.saturation);
    c = (c - 0.5) * push.contrast + 0.5;
    outColor = vec4(clamp(c, 0.0, 1.0), mix(sharp.a, blur.a, t));
}
