// Depth-of-field composite: full-res HDR sharp image + half-res blurred HDR,
// mixed per fragment by a circle-of-confusion factor computed from the
// resolved scene depth. Runs pre-tonemap so the DoF'd image feeds bloom and
// tonemap like any other HDR content. CoC ramps from 0 inside
// focusDistance +/- focusRange to 1 at +/- 2*focusRange (smoothstepped);
// the blur itself is a fixed-radius separable Gaussian at half res
// (maxBlur texels), so CoC only controls the sharp/blur mix — the accepted
// approximation at this cost tier (same family as the tilt-shift pass).

#version 330 core
in vec2 vUV;
uniform sampler2D uSharp;      // full-res HDR scene
uniform sampler2D uBlur;       // half-res blurred HDR scene
uniform sampler2D uDepthTex;   // resolved scene depth
uniform vec2  uDepthRange;     // camera near, far
uniform int   uPerspective;    // 1 = perspective projection, 0 = ortho
uniform float uFocusDistance;  // eye-space distance in perfect focus
uniform float uFocusRange;     // +/- distance around focus that stays sharp
out vec4 FragColor;

// Window-space depth [0,1] -> eye-space distance in world units.
float linearizeDepth(float d) {
    float n = uDepthRange.x;
    float f = uDepthRange.y;
    if (uPerspective == 1) {
#ifdef REVERSED_Z
        // Inverse of the reversed [0,1] mapping:
        //   z_ndc = n * (f - dist) / (dist * (f - n))
        return n * f / max(d * (f - n) + n, 1e-9);
#else
        float z = d * 2.0 - 1.0;
        return 2.0 * n * f / (f + n - z * (f - n));
#endif
    }
    return n + d * (f - n);
}

void main() {
    vec4 sharp = texture(uSharp, vUV);
    vec4 blur  = texture(uBlur, vUV);
    float dist = linearizeDepth(texture(uDepthTex, vUV).r);
    float coc = clamp((abs(dist - uFocusDistance) - uFocusRange)
                      / max(uFocusRange, 1e-3), 0.0, 1.0);
    coc = coc * coc * (3.0 - 2.0 * coc);
    FragColor = mix(sharp, blur, coc);
}
