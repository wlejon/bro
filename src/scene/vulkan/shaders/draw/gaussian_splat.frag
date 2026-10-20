#version 450
layout(location = 0) in vec2 vPos;
layout(location = 1) in vec4 vColor;
layout(location = 0) out vec4 fragColor;

void main() {
    float power = -0.5 * dot(vPos, vPos);
    float alpha = exp(power) * vColor.a;
    if (alpha < (1.0 / 255.0)) discard;
    fragColor = vec4(vColor.rgb * alpha, alpha);
}
