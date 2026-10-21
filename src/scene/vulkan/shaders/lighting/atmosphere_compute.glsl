// What the atmosphere's table makers share (scene/vulkan/lighting/scene_atmosphere.cpp):
// the block (the atmosphere, the froxel camera's inverse, the job's numbers),
// the model (common/scene_atmosphere.glsl) and the march every view table
// integrates the air with.
//
// Set 0: 0 the block, 1 the transmittance table, 2 the multiple-scattering
// table, 3 and 4 the images a job writes (declared by each shader).

#include "common/scene_atmosphere_params.glsl"

layout(set = 0, binding = 0) uniform AtmosphereBlock {
    AtmosphereGpu atm;
    mat4 froxelInvViewProj;   // the froxel camera's clip -> a position relative to its eye
    vec4 job;                 // x = the sky-view table's r, y = its mu_s
} block;
layout(set = 0, binding = 1) uniform sampler2D tTransmittance;
layout(set = 0, binding = 2) uniform sampler2D tMultiScatter;

#define ATM block.atm
#define ATM_TRANSMITTANCE_TEX tTransmittance
#include "common/scene_atmosphere.glsl"

#ifdef ATM_READS_TABLES

// The light the air scatters toward the eye at `p` (radius r, the sun
// `sun` at mu_s there), per metre, for a sun of 1: single scattering by the
// phase functions (`pR`, `pM` for the view's angle to the sun), plus the
// multiple scattering the table holds, isotropic.
vec3 atmScatterAt(vec3 p, vec3 sun, float pR, float pM, out vec3 extinction) {
    float r = length(p);
    AtmMedium m = atmMedium(r);
    extinction = m.extinction;
    float muS = dot(p, sun) / r;
    vec3 sunT = atmSunTransmittance(r, muS);
    vec3 ms = textureLod(tMultiScatter, atmMultiScatterUv(r, muS), 0.0).rgb / ATM_TABLE_SCALE;
    return (m.scatterR * pR + m.scatterM * pM) * sunT + (m.scatterR + vec3(m.scatterM)) * ms;
}

// The in-scattered radiance (sun 1, towards `sun`) along ro + rd t, t in
// [t0, t1], in `steps` equal segments, each integrated analytically for its
// medium (Hillaire's energy-conserving step); `transmittance` multiplied by
// the segment's.
vec3 atmMarch(vec3 ro, vec3 rd, vec3 sun, float t0, float t1, int steps, inout vec3 transmittance) {
    float nu = dot(rd, sun);
    float pR = atmPhaseR(nu), pM = atmPhaseM(nu, ATM.mie.z);
    float dt = (t1 - t0) / float(steps);
    vec3 L = vec3(0.0);
    for (int i = 0; i < steps; ++i) {
        vec3 p = ro + rd * (t0 + dt * (float(i) + 0.5));
        vec3 ext;
        vec3 S = atmScatterAt(p, sun, pR, pM, ext);
        vec3 stepT = exp(-ext * dt);
        vec3 Sint = mix(S * dt, (S - S * stepT) / max(ext, vec3(1e-20)), greaterThan(ext, vec3(1e-12)));
        L += transmittance * Sint;
        transmittance *= stepT;
    }
    return L;
}

// The air a ray from `ro` crosses: [t0, t1] inside the shell, stopping at
// the ground. False when it misses the air.
bool atmSegment(vec3 ro, vec3 rd, out float t0, out float t1, out bool ground) {
    t0 = atmSphereNear(ro, rd, ATM_RT);
    t1 = atmSphereFar(ro, rd, ATM_RT);
    ground = false;
    if (t0 < 0.0 || t1 <= t0) return false;
    float g = atmGroundHit(ro, rd);
    if (g >= 0.0 && g < t1) {
        t1 = g;
        ground = true;
    }
    return t1 > t0;
}
#endif
