#version 450

layout(location = 0) in vec2 inUV;
layout(location = 0) out vec4 outColor;

layout(set = 0, binding = 0) uniform sampler2D texSource;

layout(push_constant) uniform BloomPushConstants {
    float threshold;
    float knee;
    float blurRadius;
    int   passType; // 0 = threshold extraction, 1 = 9-tap gaussian blur
} push;

void main() {
    if (push.passType == 0) {
        // Bright extraction pass
        vec4 color = texture(texSource, inUV);
        float brightness = max(color.r, max(color.g, color.b));
        float soft = brightness - push.threshold + push.knee;
        soft = clamp(soft, 0.0, 2.0 * push.knee);
        soft = soft * soft / (4.0 * push.knee + 0.00001);
        float contribution = max(soft, brightness - push.threshold);
        contribution /= max(brightness, 0.00001);
        outColor = vec4(color.rgb * max(contribution, 0.0), color.a);
    } else {
        // 9-tap gaussian blur filter
        vec2 texelSize = vec2(push.blurRadius);
        vec3 result = vec3(0.0);
        float weights[5] = float[](0.227027, 0.1945946, 0.1216216, 0.054054, 0.016216);

        result += texture(texSource, inUV).rgb * weights[0];
        for (int i = 1; i < 5; ++i) {
            vec2 offset = vec2(float(i)) * texelSize;
            result += texture(texSource, inUV + offset).rgb * weights[i] * 0.5;
            result += texture(texSource, inUV - offset).rgb * weights[i] * 0.5;
            result += texture(texSource, inUV + vec2(offset.x, -offset.y)).rgb * weights[i] * 0.5;
            result += texture(texSource, inUV - vec2(offset.x, -offset.y)).rgb * weights[i] * 0.5;
        }
        outColor = vec4(result, 1.0);
    }
}
