// 3D particle fragment shader. Untextured particles are soft round points
// (quadratic radial falloff); textured particles sample straight-alpha RGBA
// (broimage decode). Output is premultiplied — the pass blends with
// (ONE, ONE_MINUS_SRC_ALPHA) for normal and (ONE, ONE) color for additive.
//
// Soft particles: when uSoftDistance > 0, alpha fades in over the world-space
// depth gap between the fragment and the opaque scene behind it, killing the
// hard clip line where a quad intersects geometry. uSceneDepth is a
// single-sampled snapshot of the opaque depth (blitted before this pass —
// sampling the draw FBO's own depth attachment is a feedback loop in GL 3.3).

#version 330 core
in vec2 vUV;
in vec4 vColor;

uniform int uMode;   // 0 = soft round point, 1 = textured
uniform sampler2D uTex;

uniform sampler2D uSceneDepth;  // opaque scene depth snapshot
uniform vec2 uViewport;         // render-target size in pixels
uniform vec2 uDepthRange;       // camera near, far
uniform int uPerspective;       // 1 = perspective projection, 0 = ortho
uniform float uSoftDistance;    // world-units fade distance; 0 = off

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
    if (uSoftDistance > 0.0) {
        float sceneD = texture(uSceneDepth, gl_FragCoord.xy / uViewport).r;
        float sceneZ = linearizeDepth(sceneD);
        float fragZ  = linearizeDepth(gl_FragCoord.z);
        a *= clamp((sceneZ - fragZ) / uSoftDistance, 0.0, 1.0);
    }
    if (a <= 0.001) discard;
    FragColor = vec4(rgb * a, a);
}
