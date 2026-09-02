// Tilt-shift DOF composite: lerps sharp->blurred by vertical distance from a
// focus band and applies a saturation/contrast boost for the miniature look.
// LDR, after tonemap. Paired with blur.frag (a 9-tap separable Gaussian,
// ping-pong, half-res). The fullscreen-quad VAO is shared with the tonemap
// pass (same `layout(location=0) in vec2 aPos`, see post.vert).

#version 330 core
in vec2 vUV;
uniform sampler2D uSharp;
uniform sampler2D uBlur;
uniform float uFocusCenter;
uniform float uFocusWidth;
uniform float uFeather;
uniform float uSaturation;
uniform float uContrast;
out vec4 FragColor;

void main() {
    vec4 sharp = texture(uSharp, vUV);
    vec4 blur  = texture(uBlur,  vUV);

    // Vertical distance from the sharp band → blur weight.
    float d = abs(vUV.y - uFocusCenter);
    float t = smoothstep(uFocusWidth, uFocusWidth + uFeather, d);
    vec3 c = mix(sharp.rgb, blur.rgb, t);

    // Miniature grade: punch chroma, then contrast around mid-gray.
    float luma = dot(c, vec3(0.299, 0.587, 0.114));
    c = mix(vec3(luma), c, uSaturation);
    c = (c - 0.5) * uContrast + 0.5;
    // Alpha rides through like the DoF composite's (blur.frag blurs all four
    // channels for exactly this): a scene canvas with no background stays
    // transparent under the tilt-shift, so the compositor can lay it over
    // the page rather than an opaque black box.
    FragColor = vec4(clamp(c, 0.0, 1.0), mix(sharp.a, blur.a, t));
}
