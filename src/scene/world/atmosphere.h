#pragma once

// The atmosphere's parameters and the CPU half of its model
// (Hillaire 2020, "A Scalable and
// Production Ready Sky and Atmosphere Rendering Technique").
//
// One model from the ground to space: a spherical shell of air between
// planetRadius and planetRadius + thickness, three media in it (Rayleigh
// scattering, Mie scattering and absorption, an ozone layer that only
// absorbs), lit by a sun of a given illuminance at the top. The GPU half
// (scene/vulkan/lighting/scene_atmosphere.h, shaders/common/scene_atmosphere.glsl)
// makes its tables from these numbers; the functions below are the tables'
// parametrisations, shared word for word with the GLSL, so a test can read
// a table back and look it up as the shaders do (bro_atmosphere_test).
//
// The defaults are Earth's (Hillaire's values: Rayleigh at 680/550/440 nm,
// an 8 km scale height, Mie haze over 1.2 km with g 0.8, ozone about 25 km).
// A different planet is a parameter change, not a different shader.

#include <algorithm>
#include <cmath>

namespace bro::scene {

struct AtmosphereParams {
    bool  enabled = false;

    // Unit vector TOWARDS the sun. False `sunDirExplicit` means it TRACKS the
    // scene's brightest directional light (the opposite of its direction), so
    // the sky and the shadows always have the same sun.
    float sunDir[3]   = {0.0f, 0.35f, -0.94f};
    bool  sunDirExplicit = false;
    // The sun's illuminance at the top of the atmosphere (lux for physical
    // units; scene units need only agree with the lights). False
    // `sunColorExplicit` means it TRACKS the scene's brightest directional
    // light (colour * intensity): that light is the sun above the air, and
    // the renderer dims it by the air's transmittance at every surface.
    float sunColor[3] = {1.0f, 1.0f, 1.0f};
    bool  sunColorExplicit = false;
    float sunAngularRadius = 0.004675f;   // radians; the real sun (0.2678 degrees)

    float planetRadius = 6360000.0f;   // metres
    float thickness    = 100000.0f;    // metres of air above the surface

    float rayleigh[3]   = {5.802e-6f, 13.558e-6f, 33.1e-6f};   // scattering per metre at sea level
    float scaleHeightR  = 8000.0f;
    float mieScattering = 3.996e-6f;
    float mieAbsorption = 0.444e-6f;
    float mieG          = 0.8f;
    float scaleHeightM  = 1200.0f;
    // Ozone: absorption only, a tent of density 1 at ozoneCenter falling to 0
    // ozoneWidth either side.
    float ozone[3]    = {0.650e-6f, 1.881e-6f, 0.085e-6f};
    float ozoneCenter = 25000.0f;
    float ozoneWidth  = 15000.0f;
    // The planet's surface, a Lambertian reflector: it lights the air (the
    // multiple scattering) and the lower hemisphere of everything on it.
    float groundAlbedo[3] = {0.3f, 0.3f, 0.3f};

    // World Y that sits on the planet's surface. Terrain built around y = 0
    // wants 0; a world whose ground sits at 1200 m wants 1200.
    //
    // The model is always spherical, but the world is a height field whose
    // up is +Y everywhere, so the planet is placed directly beneath the
    // viewer (anchoring it anywhere fixed would tilt the air's vertical away
    // from the terrain's as the viewer travelled, drawing a second sloping
    // horizon across the sky).
    float seaLevel = 0.0f;
};

// Parameters for the additive starfield (scene/vulkan/shaders/lighting/starfield.frag).
// Drawn over whichever sky is active, so the same field reads as invisible in a
// bright day sky and as stars against the near-black sky above the atmosphere.
struct StarfieldParams {
    bool  enabled   = false;
    float intensity = 1.0f;   // overall brightness
    float density   = 1.0f;   // ~0..2, relative fraction of cells that hold a star
    float rotation  = 0.0f;   // radians; slowly turns the celestial sphere about Y
};

namespace atmosphere {

// --- The tables' sizes ----------------------------------------------------------
// (common/scene_atmosphere.glsl has the same numbers.)
constexpr int kTransmittanceW = 256, kTransmittanceH = 64;    // (mu, r)
constexpr int kMultiScatterSun = 64, kMultiScatterAlt = 32;    // (mu_s, r)
constexpr int kSkyViewW = 256, kSkyViewH = 128;                // (azimuth, zenith) at the eye
constexpr int kSkyLightSun = 64, kSkyLightAlt = 32, kSkyLightLayers = 5;   // the sky's light, SH by (mu_s, altitude)
constexpr int kFroxelXY = 32, kFroxelSlices = 32;              // aerial perspective, camera-aligned

constexpr double kPi = 3.14159265358979323846;
// The tables of light (all but the transmittance) hold it times this, so
// twilight's stays above half float's smallest values.
constexpr double kTableScale = 1024.0;

// A uv in [0, 1] onto texel centres of a `size`-texel axis and back.
inline double unitToTexel(double x, int size) { return 0.5 / size + x * (1.0 - 1.0 / size); }
inline double texelToUnit(double u, int size) { return (u - 0.5 / size) / (1.0 - 1.0 / size); }

// --- The transmittance table (Bruneton 2017): (r, mu) of a ray to the top ----
inline void transmittanceUv(double Rb, double Rt, double r, double mu, double& u, double& v) {
    const double H = std::sqrt(std::max(Rt * Rt - Rb * Rb, 0.0));
    const double rho = std::sqrt(std::max(r * r - Rb * Rb, 0.0));
    const double disc = r * r * (mu * mu - 1.0) + Rt * Rt;
    const double d = std::max(-r * mu + std::sqrt(std::max(disc, 0.0)), 0.0);
    const double dMin = Rt - r, dMax = rho + H;
    const double xMu = dMax > dMin ? (d - dMin) / (dMax - dMin) : 0.0;
    const double xR = H > 0.0 ? rho / H : 0.0;
    u = unitToTexel(std::clamp(xMu, 0.0, 1.0), kTransmittanceW);
    v = unitToTexel(std::clamp(xR, 0.0, 1.0), kTransmittanceH);
}
inline void transmittanceRMu(double Rb, double Rt, double u, double v, double& r, double& mu) {
    const double xMu = texelToUnit(u, kTransmittanceW), xR = texelToUnit(v, kTransmittanceH);
    const double H = std::sqrt(std::max(Rt * Rt - Rb * Rb, 0.0));
    const double rho = H * xR;
    r = std::sqrt(rho * rho + Rb * Rb);
    const double dMin = Rt - r, dMax = rho + H;
    const double d = dMin + xMu * (dMax - dMin);
    mu = d <= 0.0 ? 1.0 : std::clamp((H * H - rho * rho - d * d) / (2.0 * r * d), -1.0, 1.0);
}

// --- The sun's angle in the (mu_s, ...) tables ------------------------------------
// The light at a point falls off steeply as the sun sets there (twilight is
// the air above lit at a grazing angle, then less and less of it), so the
// tables over mu_s put their texels near the horizon: x = 1/2 + sign(mu_s)
// sqrt(|mu_s|) / 2, and back.
inline double sunUnit(double muS) {
    const double m = std::clamp(muS, -1.0, 1.0);
    return 0.5 + 0.5 * (m < 0.0 ? -std::sqrt(-m) : std::sqrt(m));
}
inline double sunFromUnit(double x) {
    const double s = 2.0 * x - 1.0;
    return s < 0.0 ? -s * s : s * s;
}

// --- The multiple-scattering table: (mu_s, r) -----------------------------------
inline void multiScatterUv(double Rb, double Rt, double r, double muS, double& u, double& v) {
    u = unitToTexel(sunUnit(muS), kMultiScatterSun);
    v = unitToTexel(std::clamp((r - Rb) / (Rt - Rb), 0.0, 1.0), kMultiScatterAlt);
}

// --- The sky-view table at the eye: (azimuth from the sun, zenith angle) -----
// Zenith angles run from the first that sees air (0 inside the shell; the
// shell's edge from outside it) to the nadir, the horizon (the planet's
// tangent) in the middle of v, each side squeezed toward it (Hillaire's
// mapping, extended to an eye above the air); azimuth is |angle| to the
// sun's, squeezed toward the sun. `cosZ` is the view's cosine to the local
// up, `cosAz` the cosine between the view's and the sun's horizontal parts.
inline void skyViewHorizon(double Rb, double Rt, double r, double& zTop, double& zHorizon) {
    const double rr = std::max(r, Rb + 1.0);
    zHorizon = kPi - std::asin(std::min(Rb / rr, 1.0));
    zTop = rr > Rt ? kPi - std::asin(std::min(Rt / rr, 1.0)) : 0.0;
}
inline void skyViewUv(double Rb, double Rt, double r, double cosZ, double cosAz, double& u, double& v) {
    double zTop, zH;
    skyViewHorizon(Rb, Rt, r, zTop, zH);
    const double z = std::acos(std::clamp(cosZ, -1.0, 1.0));
    double y;
    if (z < zH) {
        const double c = std::clamp((z - zTop) / (zH - zTop), 0.0, 1.0);
        y = 0.5 * (1.0 - std::sqrt(1.0 - c));
    } else {
        const double c = std::clamp((z - zH) / (kPi - zH), 0.0, 1.0);
        y = 0.5 + 0.5 * std::sqrt(c);
    }
    const double x = std::sqrt(std::clamp(0.5 - 0.5 * cosAz, 0.0, 1.0));
    u = unitToTexel(x, kSkyViewW);
    v = unitToTexel(y, kSkyViewH);
}
inline void skyViewDirection(double Rb, double Rt, double r, double u, double v, double& cosZ, double& cosAz) {
    double zTop, zH;
    skyViewHorizon(Rb, Rt, r, zTop, zH);
    const double x = texelToUnit(u, kSkyViewW), y = texelToUnit(v, kSkyViewH);
    double z;
    if (y < 0.5) {
        const double c = 1.0 - (1.0 - 2.0 * y) * (1.0 - 2.0 * y);
        z = zTop + c * (zH - zTop);
    } else {
        const double c = (2.0 * y - 1.0) * (2.0 * y - 1.0);
        z = zH + c * (kPi - zH);
    }
    cosZ = std::cos(z);
    cosAz = 1.0 - 2.0 * x * x;
}

// --- The sky's light: SH by (mu_s, altitude) -------------------------------------
inline void skyLightUv(double thickness, double h, double muS, double& u, double& v) {
    u = unitToTexel(sunUnit(muS), kSkyLightSun);
    v = unitToTexel(std::sqrt(std::clamp(h / thickness, 0.0, 1.0)), kSkyLightAlt);
}

// --- The aerial-perspective froxels ---------------------------------------------
// Slice k (0..kFroxelSlices-1) holds the air from the eye to its far edge,
// at distance maxDist * ((k + 1) / slices)^2: fine near the eye, coarse far.
inline double froxelSliceDistance(double maxDist, int k) {
    const double s = static_cast<double>(k + 1) / kFroxelSlices;
    return maxDist * s * s;
}
// The volume's w for a distance (its texel centres are the slices), and the
// share of the first slice's air a point nearer than it gets.
inline double froxelW(double maxDist, double d, double& nearWeight) {
    const double s = std::sqrt(std::max(d, 0.0) / maxDist) * kFroxelSlices;   // slice k at s = k + 1
    nearWeight = std::clamp(d / froxelSliceDistance(maxDist, 0), 0.0, 1.0);
    return std::clamp((s - 0.5) / kFroxelSlices, 0.5 / kFroxelSlices, 1.0 - 0.5 / kFroxelSlices);
}

// --- SH (real, l <= 2) of the sky's light, in the sun's frame ------------------
// The frame's z is the local up, x the sun's horizontal direction; the sky is
// symmetric across that plane, so only the six coefficients even in y are
// kept: Y00, Y10 (z), Y11 (x), Y20, Y21 (xz), Y22 (x^2 - y^2).
inline void shBasis6(double x, double y, double z, double out[6]) {
    out[0] = 0.282095;
    out[1] = 0.488603 * z;
    out[2] = 0.488603 * x;
    out[3] = 0.315392 * (3.0 * z * z - 1.0);
    out[4] = 1.092548 * x * z;
    out[5] = 0.546274 * (x * x - y * y);
}
// Irradiance over pi (average incident radiance) on a surface facing (x, y, z)
// from radiance coefficients `c` (6 per channel): Ramamoorthi & Hanrahan's
// cosine-lobe weights pi, 2pi/3, pi/4 per band.
inline double shIrradianceOverPi(const double c[6], double x, double y, double z) {
    double b[6];
    shBasis6(x, y, z, b);
    const double e = kPi * c[0] * b[0] + (2.0 * kPi / 3.0) * (c[1] * b[1] + c[2] * b[2]) +
                     (kPi / 4.0) * (c[3] * b[3] + c[4] * b[4] + c[5] * b[5]);
    return std::max(e, 0.0) / kPi;
}

}  // namespace atmosphere

}  // namespace bro::scene
