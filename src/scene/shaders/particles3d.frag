// 3D particle fragment shader. Untextured particles are soft round points
// (quadratic radial falloff); textured particles sample straight-alpha RGBA
// (broimage decode). Output is premultiplied — the pass blends with
// (ONE, ONE_MINUS_SRC_ALPHA) for normal and (ONE, ONE) color for additive.

#version 330 core
in vec2 vUV;
in vec4 vColor;

uniform int uMode;   // 0 = soft round point, 1 = textured
uniform sampler2D uTex;

out vec4 FragColor;

void main() {
    float a;
    vec3 rgb;
    if (uMode == 1) {
        vec4 tex = texture(uTex, vUV);
        a = tex.a * vColor.a;
        rgb = tex.rgb * vColor.rgb;
    } else {
        vec2 p = vUV - 0.5;
        float d = length(p) * 2.0;
        float t = clamp(1.0 - d, 0.0, 1.0);
        a = vColor.a * t * t;
        rgb = vColor.rgb;
    }
    if (a <= 0.001) discard;
    FragColor = vec4(rgb * a, a);
}
