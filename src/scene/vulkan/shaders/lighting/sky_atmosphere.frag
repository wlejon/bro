#version 450
// The atmosphere's sky (docs/scene-world-architecture.md §7): the sky-view
// table's radiance along the view (made at the eye's height and sun this
// frame, scene_atmosphere.cpp), the lookup dithered by a fraction of a texel
// so its bilinear steps do not band; where the view meets the planet, the
// ground lit by the sun and the sky through the air before it; and the sun's
// disc, its light what the air leaves of it (the transmittance table), so
// it reddens and dims toward the horizon, its limb darkened.

#include "lighting/sky_common.glsl"

// The limb darkening of the sun's disc, I(mu) / I(1) = 1 - u (1 - mu) (the
// visible band's mean); the disc's radiance is normalised so its
// illuminance is the sun's.
const float SUN_LIMB_U = 0.6;

void main() {
    outMark = 0.0;
    outIndirect = vec4(0.0);
    vec3 rd = normalize(inWorldDir);
    vec3 E = lighting.atm.sunIlluminance.rgb;
    vec3 ro = atmOrigin(vec3(0.0));
    float r = length(ro);
    vec3 up = ro / r, xAxis, yAxis;
    float muS;
    atmSunFrame(up, xAxis, yAxis, muS);
    vec2 h = vec2(dot(rd, xAxis), dot(rd, yAxis));
    float hl = length(h);
    float cosAz = hl > 1e-5 ? h.x / hl : 1.0;
    vec2 uv = atmSkyViewUv(lighting.atm.skyView.x, dot(rd, up), cosAz);
    // A texel's worth of dither along both axes: the table's bilinear
    // interpolation is smooth, but its slope changes at every texel, and
    // the eye picks out a slow gradient's creases.
    vec2 jitter = (vec2(atmNoise(gl_FragCoord.xy), atmNoise(gl_FragCoord.yx + 17.0)) - 0.5) * lighting.atm.skyView.z;
    // From above the shell, a view that misses it sees space: nothing, not
    // the table's edge texels (which the exposure would meter as light).
    vec3 col = atmSphereNear(ro, rd, ATM_RT) < 0.0
                   ? vec3(0.0)
                   : textureLod(texAtmSkyView, uv + jitter / ATM_SKYVIEW_SIZE, 0.0).rgb * (E / ATM_TABLE_SCALE);

    float g = atmGroundHit(ro, rd);
    if (g >= 0.0) {
        // The planet's surface: a Lambertian of the ground's albedo, lit by
        // the sun and by the sky over it, seen through the air.
        vec3 p = ro + rd * g;
        vec3 n = normalize(p);
        float mu = dot(n, lighting.atm.sunDir.xyz);
        vec3 sun = atmSunTransmittance(ATM_RB, mu) * max(mu, 0.0) * E / ATM_PI;
        vec3 sky = sceneSkyIrradiance(p - ro, n);
        vec3 a = atmTransmittanceToTop(ATM_RB, -dot(n, rd));
        float rIn = min(r, ATM_RT);
        vec3 eyeIn = r > ATM_RT ? ro + rd * atmSphereNear(ro, rd, ATM_RT) : ro;
        vec3 b = atmTransmittanceToTop(rIn,-dot(eyeIn, rd) / rIn);
        // The ground's ray back up passes the eye: a = T(ground..eye) * b.
        vec3 T = clamp(a / max(b, vec3(1e-6)), 0.0, 1.0);
        col += T * lighting.atm.ground.rgb * (sun + sky);
    } else {
        float alpha = max(lighting.atm.sunIlluminance.w, 1e-4);
        float c = dot(rd, lighting.atm.sunDir.xyz);
        float cosAlpha = cos(alpha);
        if (c > cosAlpha) {
            float x = clamp(acos(min(c, 1.0)) / alpha, 0.0, 1.0);
            float limb = (1.0 - SUN_LIMB_U * (1.0 - sqrt(1.0 - x * x))) / (1.0 - SUN_LIMB_U / 3.0);
            vec3 L = E / (ATM_PI * alpha * alpha) * limb;
            col += L * atmTransmittanceRay(ro, rd);
        }
    }
    outColor = vec4(min(col, vec3(SCENE_HDR_MAX)), 1.0);
}
