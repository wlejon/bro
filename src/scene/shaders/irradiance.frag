// Irradiance convolution: integrate the env cubemap over a cosine-weighted
// hemisphere around each output texel's normal. The result feeds diffuse
// IBL: `Ld = albedo * irradiance(N) / PI`. Diffuse is low-frequency so the
// output cube can be tiny (32^2 is plenty); the cost is per-fragment Riemann
// integration which dominates load time but is one-shot per HDR.

#version 330 core
in vec2 vUV;
out vec4 FragColor;

uniform samplerCube uEnv;
uniform int         uFace;

const float PI     = 3.14159265358979;
const float TWO_PI = 6.28318530717958;

vec3 cubeDir(int face, vec2 uv) {
    if (face == 0) return normalize(vec3( 1.0, -uv.y, -uv.x));
    if (face == 1) return normalize(vec3(-1.0, -uv.y,  uv.x));
    if (face == 2) return normalize(vec3( uv.x,  1.0,  uv.y));
    if (face == 3) return normalize(vec3( uv.x, -1.0, -uv.y));
    if (face == 4) return normalize(vec3( uv.x, -uv.y,  1.0));
    return            normalize(vec3(-uv.x, -uv.y, -1.0));
}

void main() {
    vec2 uv = vUV * 2.0 - 1.0;
    vec3 N  = cubeDir(uFace, uv);

    // Build a tangent basis around N. Picking up = world-Y unless N is
    // near-parallel to it (then up = world-Z to keep the cross non-degenerate).
    vec3 up    = abs(N.y) < 0.999 ? vec3(0.0, 1.0, 0.0) : vec3(0.0, 0.0, 1.0);
    vec3 right = normalize(cross(up, N));
    up         = cross(N, right);

    // Riemann hemisphere integration. sampleDelta=0.025 → ~252×63 = 15876
    // samples per fragment; coarse but robust for an offline pass. Both
    // sums of cos*sin and the normalising 1/N cancel into the PI factor.
    vec3 irradiance = vec3(0.0);
    int  nSamples   = 0;
    const float sampleDelta = 0.025;
    for (float phi = 0.0; phi < TWO_PI; phi += sampleDelta) {
        for (float theta = 0.0; theta < 0.5 * PI; theta += sampleDelta) {
            // Spherical → cartesian in tangent space.
            vec3 t = vec3(sin(theta) * cos(phi),
                          sin(theta) * sin(phi),
                          cos(theta));
            vec3 sampleVec = t.x * right + t.y * up + t.z * N;
            // Reject NaN/Inf and clamp tiny negatives from upstream processing
            // so the accumulation stays well-defined and non-negative.
            vec3 s = texture(uEnv, sampleVec).rgb;
            s = max(s, vec3(0.0));
            if (any(isnan(s)) || any(isinf(s))) continue;
            irradiance += s * cos(theta) * sin(theta);
            nSamples++;
        }
    }
    irradiance = nSamples > 0 ? PI * irradiance / float(nSamples) : vec3(0.0);
    FragColor = vec4(irradiance, 1.0);
}
