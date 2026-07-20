#pragma once

// Sky irradiance on the CPU — the ambient term the atmosphere implies.
//
// A flat ambient constant is what makes lit snow blow out to white: the surface
// is told to reflect a uniform white sky no matter where the sun is or how much
// air is above it. The sky pass already knows the real answer, so this integrates
// the SAME model (src/scene/shaders/atmosphere.glsl) over the upper hemisphere
// and hands the result to the mesh shaders as a plain vec3.
//
// On the CPU and once per frame rather than per pixel, because sky irradiance
// varies only with camera altitude and sun angle — both uniform across the
// scene. A per-pixel hemisphere march would cost thousands of times more to
// produce a value that is constant over the frame.
//
// Returns average incident radiance (irradiance / pi), which is the quantity
// mesh.frag's `uAmbient * baseColor` already expects.

#include <cmath>

#include "scene/atmosphere.h"

namespace bro::scene {

namespace atm_detail {

inline float exitDistance(const float ro[3], const float rd[3], float R) {
    const float b = ro[0] * rd[0] + ro[1] * rd[1] + ro[2] * rd[2];
    const float c = ro[0] * ro[0] + ro[1] * ro[1] + ro[2] * ro[2] - R * R;
    const float d = b * b - c;
    if (d < 0.0f) return -1.0f;
    return -b + std::sqrt(d);
}

inline float hitDistance(const float ro[3], const float rd[3], float R) {
    const float b = ro[0] * rd[0] + ro[1] * rd[1] + ro[2] * rd[2];
    const float c = ro[0] * ro[0] + ro[1] * ro[1] + ro[2] * ro[2] - R * R;
    const float d = b * b - c;
    if (d < 0.0f) return -1.0f;
    const float t = -b - std::sqrt(d);
    return t >= 0.0f ? t : -1.0f;
}

// Integrated Rayleigh/Mie density along a ray. Mirrors atmOpticalDepth().
inline void opticalDepth(const AtmosphereParams& a, const float ro[3],
                         const float rd[3], float tMax, int steps, float out[2]) {
    const float dt = tMax / static_cast<float>(steps);
    out[0] = out[1] = 0.0f;
    for (int i = 0; i < steps; ++i) {
        const float t = dt * (static_cast<float>(i) + 0.5f);
        const float px = ro[0] + rd[0] * t;
        const float py = ro[1] + rd[1] * t;
        const float pz = ro[2] + rd[2] * t;
        float h = std::sqrt(px * px + py * py + pz * pz) - a.planetRadius;
        if (h < 0.0f) h = 0.0f;
        out[0] += std::exp(-h / a.scaleHeightR);
        out[1] += std::exp(-h / a.scaleHeightM);
    }
    out[0] *= dt;
    out[1] *= dt;
}

inline void extinction(const AtmosphereParams& a, const float od[2], float out[3]) {
    for (int c = 0; c < 3; ++c) {
        out[c] = std::exp(-(a.betaR[c] * od[0] + a.betaM * 1.1f * od[1]));
    }
}

inline float phaseR(float mu) {
    return 3.0f / (16.0f * 3.14159265f) * (1.0f + mu * mu);
}

inline float phaseM(float mu, float g) {
    const float g2 = g * g;
    float d = 1.0f + g2 - 2.0f * g * mu;
    if (d < 1e-4f) d = 1e-4f;
    return 3.0f / (8.0f * 3.14159265f) * ((1.0f - g2) * (1.0f + mu * mu))
         / ((2.0f + g2) * std::pow(d, 1.5f));
}

// Single-scattered sky radiance along one ray. Mirrors atmScatter() + atmSky().
inline void skyRadiance(const AtmosphereParams& a, const float ro[3],
                        const float rd[3], int steps, int sunSteps, float out[3]) {
    out[0] = out[1] = out[2] = 0.0f;

    const float atmR = a.planetRadius + a.thickness;
    float tMax = exitDistance(ro, rd, atmR);
    if (tMax <= 0.0f) return;
    const float tGround = hitDistance(ro, rd, a.planetRadius);
    if (tGround > 0.0f && tGround < tMax) tMax = tGround;

    const float sun[3] = {a.sunDir[0], a.sunDir[1], a.sunDir[2]};
    const float mu = rd[0] * sun[0] + rd[1] * sun[1] + rd[2] * sun[2];
    const float pr = phaseR(mu);
    const float pm = phaseM(mu, a.mieG);

    const float dt = tMax / static_cast<float>(steps);
    float odView[2] = {0.0f, 0.0f};
    float sumR[3] = {0.0f, 0.0f, 0.0f};
    float sumM = 0.0f;

    for (int i = 0; i < steps; ++i) {
        const float t = dt * (static_cast<float>(i) + 0.5f);
        const float p[3] = {ro[0] + rd[0] * t, ro[1] + rd[1] * t, ro[2] + rd[2] * t};
        float h = std::sqrt(p[0] * p[0] + p[1] * p[1] + p[2] * p[2]) - a.planetRadius;
        if (h < 0.0f) h = 0.0f;
        const float dR = std::exp(-h / a.scaleHeightR) * dt;
        const float dM = std::exp(-h / a.scaleHeightM) * dt;
        odView[0] += dR;
        odView[1] += dM;

        float sunTrans[3] = {0.0f, 0.0f, 0.0f};
        if (hitDistance(p, sun, a.planetRadius) < 0.0f) {
            const float ts = exitDistance(p, sun, atmR);
            if (ts > 0.0f) {
                float odSun[2];
                opticalDepth(a, p, sun, ts, sunSteps, odSun);
                const float od[2] = {odSun[0] + odView[0], odSun[1] + odView[1]};
                extinction(a, od, sunTrans);
            }
        }
        for (int c = 0; c < 3; ++c) sumR[c] += sunTrans[c] * dR;
        sumM += (sunTrans[0] + sunTrans[1] + sunTrans[2]) / 3.0f * dM;
    }

    for (int c = 0; c < 3; ++c) {
        out[c] = a.sunColor[c] * (a.betaR[c] * sumR[c] * pr + a.betaM * sumM * pm);
    }
}

}  // namespace atm_detail

// Cosine-weighted average sky radiance over the upper hemisphere at `camY`.
//
// Feeding this to uAmbient makes ambient track the sky the surface is actually
// under: blue at midday, dim and orange near sunset, and thinning toward black
// as the camera climbs out of the atmosphere — none of which a constant can do.
inline void computeSkyAmbient(const AtmosphereParams& a, float camY, float out[3]) {
    out[0] = out[1] = out[2] = 0.0f;

    // Coarse: irradiance is a hemisphere average, so it converges far faster
    // than the sky image does. 3x6 directions is visually stable under a moving
    // sun and costs a few thousand flops a frame.
    const int kTheta = 3, kPhi = 6;
    const int kViewSteps = 8, kSunSteps = 4;

    const float ro[3] = {0.0f, camY - a.seaLevel + a.planetRadius, 0.0f};
    const float kPi = 3.14159265f;
    const float dTheta = (kPi * 0.5f) / static_cast<float>(kTheta);
    const float dPhi = (2.0f * kPi) / static_cast<float>(kPhi);

    float sum[3] = {0.0f, 0.0f, 0.0f};
    for (int j = 0; j < kTheta; ++j) {
        const float theta = (static_cast<float>(j) + 0.5f) * dTheta;
        const float ct = std::cos(theta), st = std::sin(theta);
        for (int k = 0; k < kPhi; ++k) {
            const float phi = (static_cast<float>(k) + 0.5f) * dPhi;
            // Hemisphere around +Y (up).
            const float rd[3] = {st * std::cos(phi), ct, st * std::sin(phi)};
            float L[3];
            atm_detail::skyRadiance(a, ro, rd, kViewSteps, kSunSteps, L);
            // dE = L * cos(theta) * sin(theta) * dTheta * dPhi
            const float w = ct * st * dTheta * dPhi;
            for (int c = 0; c < 3; ++c) sum[c] += L[c] * w;
        }
    }

    // Irradiance -> average incident radiance, which is what uAmbient means.
    for (int c = 0; c < 3; ++c) out[c] = sum[c] / kPi;
}

}  // namespace bro::scene
