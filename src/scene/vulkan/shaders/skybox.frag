#version 450
// The environment as the background: its radiance cube along the view ray,
// turned by the environment's rotation and scaled by its intensity.

#include "sky_common.glsl"

void main() {
    outIndirect = vec4(0.0);
    vec3 d = rotateY(normalize(inWorldDir), lighting.iblParams.z);
    outColor = vec4(texture(texEnvironment, d).rgb * lighting.iblParams.y, 1.0);
}
