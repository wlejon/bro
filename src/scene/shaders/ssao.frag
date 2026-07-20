// SSAO pass: half-res ambient-occlusion estimate from the resolved scene
// depth (no G-buffer in a forward renderer — view-space position is
// reconstructed from depth + the inverse projection, normals from position
// derivatives). Classic hemisphere-kernel SSAO (Crytek/LearnOpenGL style):
// 16 kernel samples oriented by a 4x4 tiling rotation-noise texture, range
// check so distant geometry doesn't occlude across depth discontinuities.
// Output is a single AO factor in R (1 = open, 0 = fully occluded); a
// separable blur then erases the 4x4 noise pattern before the tonemap pass
// multiplies the AO into the lit HDR image.

#version 330 core
#define KERNEL_SIZE 16

in vec2 vUV;
uniform sampler2D uDepthTex;    // resolved scene depth (full-res)
uniform sampler2D uNoiseTex;    // 4x4 rotation vectors, tiles across screen
uniform mat4  uProj;            // camera projection
uniform mat4  uInvProj;         // inverse projection
uniform vec3  uKernel[KERNEL_SIZE];
uniform float uRadius;          // world-unit hemisphere radius
uniform float uBias;            // depth acceptance bias (view-space units)
uniform vec2  uNoiseScale;      // AO resolution / 4
out vec4 FragColor;

// Window-space UV + depth -> view-space position.
vec3 viewPos(vec2 uv) {
    float d = texture(uDepthTex, uv).r;
    // Window depth is already the clip-space z under [0,1] clip control.
#ifdef REVERSED_Z
    vec4 clip = vec4(uv * 2.0 - 1.0, d, 1.0);
#else
    vec4 clip = vec4(uv * 2.0 - 1.0, d * 2.0 - 1.0, 1.0);
#endif
    vec4 v = uInvProj * clip;
    return v.xyz / v.w;
}

void main() {
    float d0 = texture(uDepthTex, vUV).r;
#ifdef REVERSED_Z
    if (d0 <= 0.0) {   // background / sky: never occluded
#else
    if (d0 >= 1.0) {   // background / sky: never occluded
#endif
        FragColor = vec4(1.0);
        return;
    }
    vec3 P = viewPos(vUV);
    // Face normal from position derivatives. Sharp creases are exactly what
    // AO wants here; the half-res buffer + blur hide the faceting this
    // introduces on curved surfaces.
    vec3 N = normalize(cross(dFdx(P), dFdy(P)));

    // Per-fragment random kernel rotation (tiled 4x4 noise).
    vec3 rnd = vec3(texture(uNoiseTex, vUV * uNoiseScale).xy * 2.0 - 1.0, 0.0);
    vec3 T = normalize(rnd - N * dot(rnd, N));
    vec3 B = cross(N, T);
    mat3 TBN = mat3(T, B, N);

    float occlusion = 0.0;
    for (int i = 0; i < KERNEL_SIZE; ++i) {
        vec3 sp = P + TBN * uKernel[i] * uRadius;
        vec4 off = uProj * vec4(sp, 1.0);
        off.xyz /= off.w;
        vec2 suv = off.xy * 0.5 + 0.5;
        if (suv.x < 0.0 || suv.x > 1.0 || suv.y < 0.0 || suv.y > 1.0) continue;
        float sampleZ = viewPos(suv).z;
        // Range check: geometry further than one radius from the fragment
        // can't occlude it (stops distant silhouettes darkening everything).
        float rangeCheck = smoothstep(0.0, 1.0, uRadius / abs(P.z - sampleZ));
        occlusion += (sampleZ >= sp.z + uBias ? 1.0 : 0.0) * rangeCheck;
    }
    float ao = 1.0 - occlusion / float(KERNEL_SIZE);
    FragColor = vec4(ao, ao, ao, 1.0);
}
