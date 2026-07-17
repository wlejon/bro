// Tonemap pass: HDR float mesh FBO -> LDR RGBA8 output texture.

#version 330 core
in vec2 vUV;
uniform sampler2D uTex;
uniform sampler2D uBloomTex;
uniform float uBloomIntensity;   // 0 = bloom off
uniform sampler2D uSSAOTex;      // blurred half-res AO (R channel)
uniform float uSSAOIntensity;    // 0 = SSAO off
uniform sampler3D uLUTTex;       // 3D color-grading LUT
uniform float uLUTAmount;        // 0 = LUT off, 1 = fully graded
uniform float uLUTScale;         // (size-1)/size — texel-center mapping
uniform float uLUTOffset;        // 0.5/size
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
    // SSAO multiplies the lit HDR image (post-multiply — standard for a
    // forward renderer with no G-buffer); bloom is added AFTER so glow
    // isn't occluded. Guarded so intensity 0 leaves src bit-exact.
    if (uSSAOIntensity > 0.0) {
        src.rgb *= mix(1.0, texture(uSSAOTex, vUV).r, uSSAOIntensity);
    }
    // Add the blurred bright-pass in HDR so highlights bloom before tonemap.
    vec3 hdr = src.rgb + texture(uBloomTex, vUV).rgb * uBloomIntensity;
    vec3 c = hdr * uExposure;
    if (uMode == 2)      c = aces(c);
    else if (uMode == 1) c = c / (c + vec3(1.0));
    else                 c = clamp(c, 0.0, 1.0);
    if (uGamma > 0.0 && uGamma != 1.0) {
        c = pow(c, vec3(1.0 / uGamma));
    }
    // 3D color-grading LUT, applied AFTER tonemapping + gamma (LUTs are
    // authored in display space). Trilinear; the scale/offset remap [0,1]
    // onto texel centers so black and white hit the outer LUT nodes exactly.
    // Guarded so amount 0 leaves the frame bit-exact.
    if (uLUTAmount > 0.0) {
        vec3 graded = texture(uLUTTex, c * uLUTScale + uLUTOffset).rgb;
        c = mix(c, graded, uLUTAmount);
    }
    FragColor = vec4(c, src.a);
}
