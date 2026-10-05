#version 450
// Irradiance convolution: the environment integrated over the cosine-weighted
// hemisphere around each texel's normal, for diffuse image-based lighting.
// Low frequency, so the output cube is tiny; one Riemann pass per load.

#include "ibl_common.glsl"

layout(set = 0, binding = 0) uniform samplerCube texEnv;
layout(location = 0) out vec4 outColor;

void main() {
    vec3 N = cubeDir(bake.face, inUV * 2.0 - 1.0);
    vec3 up = abs(N.y) < 0.999 ? vec3(0.0, 1.0, 0.0) : vec3(0.0, 0.0, 1.0);
    vec3 right = normalize(cross(up, N));
    up = cross(N, right);

    // sampleDelta 0.025: about 252 x 63 samples per texel.
    vec3 irradiance = vec3(0.0);
    int samples = 0;
    const float sampleDelta = 0.025;
    for (float phi = 0.0; phi < TWO_PI; phi += sampleDelta) {
        for (float theta = 0.0; theta < 0.5 * PI; theta += sampleDelta) {
            vec3 t = vec3(sin(theta) * cos(phi), sin(theta) * sin(phi), cos(theta));
            vec3 s = sanitize(texture(texEnv, t.x * right + t.y * up + t.z * N).rgb);
            irradiance += s * cos(theta) * sin(theta);
            samples++;
        }
    }
    outColor = vec4(samples > 0 ? PI * irradiance / float(samples) : vec3(0.0), 1.0);
}
