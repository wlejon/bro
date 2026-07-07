// Tonemap pass: HDR float mesh FBO -> LDR RGBA8 output texture.

#version 330 core
in vec2 vUV;
uniform sampler2D uTex;
uniform sampler2D uBloomTex;
uniform float uBloomIntensity;   // 0 = bloom off
uniform float uExposure;
uniform float uGamma;
uniform int   uMode;   // 0 = linear clamp, 1 = Reinhard, 2 = ACES
out vec4 FragColor;

// ACES approximation by Krzysztof Narkowicz.
vec3 aces(vec3 x) {
    const float a = 2.51;
    const float b = 0.03;
    const float c = 2.43;
    const float d = 0.59;
    const float e = 0.14;
    return clamp((x * (a * x + b)) / (x * (c * x + d) + e), 0.0, 1.0);
}

void main() {
    vec4 src = texture(uTex, vUV);
    // Add the blurred bright-pass in HDR so highlights bloom before tonemap.
    vec3 hdr = src.rgb + texture(uBloomTex, vUV).rgb * uBloomIntensity;
    vec3 c = hdr * uExposure;
    if (uMode == 2)      c = aces(c);
    else if (uMode == 1) c = c / (c + vec3(1.0));
    else                 c = clamp(c, 0.0, 1.0);
    if (uGamma > 0.0 && uGamma != 1.0) {
        c = pow(c, vec3(1.0 / uGamma));
    }
    FragColor = vec4(c, src.a);
}
