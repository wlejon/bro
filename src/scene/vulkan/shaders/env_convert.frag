#version 450
// Equirectangular HDR -> one face of the environment cube. The panorama is
// uploaded top-down (row 0 = the +Y pole), hence v = 0.5 - theta / PI.

#include "ibl_common.glsl"

layout(set = 0, binding = 0) uniform sampler2D texEquirect;
layout(location = 0) out vec4 outColor;

void main() {
    vec3 d = cubeDir(bake.face, inUV * 2.0 - 1.0);
    float phi = atan(d.z, d.x);
    float theta = asin(clamp(d.y, -1.0, 1.0));
    vec2 eq = vec2(phi / TWO_PI + 0.5, 0.5 - theta / PI);
    outColor = vec4(sanitize(texture(texEquirect, eq).rgb), 1.0);
}
