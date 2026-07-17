
#version 330 core
in vec2 vUV;
uniform sampler2D uTex;
uniform vec2 uDir;        // texel step * radius, along blur axis
out vec4 FragColor;

// Normalized 9-tap Gaussian (sigma ~ 2).
const float w0 = 0.2270270270;
const float w1 = 0.1945945946;
const float w2 = 0.1216216216;
const float w3 = 0.0540540541;
const float w4 = 0.0162162162;

void main() {
    // Blur all four channels: bloom/tilt/SSAO consumers read only rgb (or
    // just r), while the DoF pre-pass needs the blurred scene ALPHA so a
    // defocused region keeps its transparency for the compositor.
    vec4 c = texture(uTex, vUV) * w0;
    c += texture(uTex, vUV + uDir * 1.0) * w1;
    c += texture(uTex, vUV - uDir * 1.0) * w1;
    c += texture(uTex, vUV + uDir * 2.0) * w2;
    c += texture(uTex, vUV - uDir * 2.0) * w2;
    c += texture(uTex, vUV + uDir * 3.0) * w3;
    c += texture(uTex, vUV - uDir * 3.0) * w3;
    c += texture(uTex, vUV + uDir * 4.0) * w4;
    c += texture(uTex, vUV - uDir * 4.0) * w4;
    FragColor = c;
}
