// Bloom bright-pass: keep the HDR energy above a luminance threshold, soft
// knee, write HDR. Blurred afterward with the shared separable Gaussian
// (blur.frag) and added back in the tonemap pass. Shares post.vert.

#version 330 core
in vec2 vUV;
uniform sampler2D uTex;
uniform float uThreshold;
out vec4 FragColor;

void main() {
    vec3 c = texture(uTex, vUV).rgb;
    float luma = dot(c, vec3(0.2126, 0.7152, 0.0722));
    // Soft knee around the threshold so the bloom onset isn't a hard edge.
    float k = clamp((luma - uThreshold) / max(uThreshold, 1e-3), 0.0, 1.0);
    FragColor = vec4(c * k, 1.0);
}
