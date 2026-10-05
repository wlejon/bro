#version 450
// The analytic sky: the view ray integrated through the atmosphere
// (scene_atmosphere.glsl), plus the sun's disk attenuated by the air in front
// of it, which is what turns it orange at the horizon.

#include "sky_common.glsl"

// The sky is one full-screen pass, so it affords more steps than the aerial
// perspective on geometry does.
const int SKY_STEPS = 24;
const int SKY_SUN_STEPS = 6;

void main() {
    outIndirect = vec4(0.0);
    vec3 rd = normalize(inWorldDir);
    vec3 eye = camera.eyePos.xyz;
    vec3 col = atmSky(eye, rd, SKY_STEPS, SKY_SUN_STEPS);

    float mu = dot(rd, uAtmSunDir);
    if (mu > cos(lighting.atmParams2.w)) {
        vec3 ro = atmOrigin(eye);
        if (atmHitDistance(ro, rd, uAtmPlanetRadius) < 0.0) {
            float t = atmExitDistance(ro, rd, uAtmPlanetRadius + uAtmThickness);
            if (t > 0.0) {
                col += uAtmSunColor * lighting.atmCenter.w * atmExtinction(atmOpticalDepth(ro, rd, t, SKY_SUN_STEPS));
            }
        }
    }
    outColor = vec4(col, 1.0);
}
