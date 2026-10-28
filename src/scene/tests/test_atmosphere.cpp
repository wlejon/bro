// bro_atmosphere_test: the atmosphere's tables (docs/scene-world-architecture.md
// §7) against a CPU reference (tests/atmosphere_reference.h).
//
// A scene renders with the atmosphere on, its eye at 2 m, 2 km, 30 km and
// 400 km, its sun at noon (68 degrees up), 10 degrees and -4 degrees
// (twilight); and in a clear day's haze (turbidity 10) at 2 m and 2 km under
// suns at 68, 35 and 10 degrees. Each render's tables are read back and looked up on the CPU
// as the shaders look them up (scene/world/atmosphere.h's parametrisations,
// bilinear like the samplers), then compared with the reference:
//
//  - transmittance: the table along rays to the top, against a 4000-step
//    march;
//  - sky radiance: the sky-view table at the eye over a dense grid, zenith
//    angles across its whole domain (the shell's edge to the nadir, the
//    zenith cap finely) and azimuths every 5 degrees from the sun, finely
//    about it;
//  - the sky as drawn: the sky pass's pixels in three views (at the sun,
//    straight up, away from it), so the shader's own direction -> table
//    mapping, the sun's azimuth either side of the view and the dither are
//    checked too; 1.5 codes of 8-bit step and dither, and the reference's
//    own spread over the half texel the lookup is dithered by, are allowed
//    before the relative error, with a floor of 2% of the view's peak;
//  - aerial perspective: the froxel volume's in-scattered light and
//    transmittance at columns across the frustum and slices near to far;
//  - the sky's light: the SH table's irradiance on an upward surface at the
//    ground, against the reference sky integrated over the hemisphere.
//
// The tolerances, and why (the errors are relative, of luminance, with a
// floor of 0.1% of the case's brightest sample so black space is not divided
// by zero):
//  - transmittance 1%: the table is a 256-step march of the same media at
//    half float (3 significant digits), and rays within a degree of the
//    horizon, where it changes fastest, are left out.
//  - in-scattered light (sky radiance and aerial perspective): 12% at any
//    sample and 8% on average by day, 30% and 15% in twilight. What is
//    left is the multiple scattering's form: Hillaire's table is one
//    isotropic geometric series (the second order's light over 1 - f_ms,
//    the ground returning its albedo of it), where the reference iterates
//    every order with the true geometry. The test prints both tables side
//    by side: they part by up to 13% by day (the table high under a high
//    sun, low under a low one), and by up to 20% at the -4 degree sun.
//    Single scattering is most of a day sky, so its error there is a few
//    percent; near the ground in twilight the light is all multiple
//    scattering and shows the whole departure. Equal-step view rays (48
//    steps for the sky, 8 a froxel slice) add under a percent.
//    In a clear day's haze (turbidity 10) more of the light is scattered
//    more than once, by the Mie haze's forward lobe, which the table's
//    isotropic series fits worse: its multiple-scattering table parts from
//    the reference's by up to 25% (at 2 km under a 10 degree sun), and the
//    day's tolerance there is 16% at any sample and 9% on average. Its
//    transmittance is 1.5%: the hazier air leaves 6e-4 of the light along
//    the rays nearest the horizon, at the edge of half float's precision.
//  - aerial transmittance 2%: the same media, 8 steps a slice.
//  - the sky's light 15%: an l <= 2 SH of the whole sphere (the ground's
//    lower half included) rings onto an upward surface's cosine lobe, on
//    top of the multiple scattering above.
//
// assert() is not the check here; a failed comparison prints and fails the run.

#include "atmosphere_reference.h"

#include "scene/scene_graph.h"
#include "scene/scene_renderer.h"
#include "scene/vulkan/lighting/scene_atmosphere.h"
#include "scene/world/atmosphere.h"
#include "render/vulkan_context.h"
#include "render/vulkan_debug.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <initializer_list>
#include <vector>

using namespace bro;
namespace atm = bro::scene::atmosphere;
using bro::scene::atmref::RGB;
using bro::scene::atmref::V3;

namespace {

constexpr double kPi = 3.14159265358979323846;
// Directions within 0.1 degree of the horizon are left out of the sky's
// checks: the sky and the ground meet there in one texel row of the sky-view
// table, and the sampler blends the two across it (the drawn sky puts the
// ground's own pixels over that band).
constexpr double kHorizonBand = 0.1 * kPi / 180.0;
int failures = 0;

// A frame begun as the engine's are (Engine::beginGpuFrame): its slot of the
// frame ring recycled, so what the scenes of earlier cases deferred is
// destroyed (MoltenVK makes no more than some 32 timestamp pools at once).
void renderFrame(scene::SceneGraph& graph) {
    scene::SceneRenderer::defaultVulkanContext()->frames().beginFrame();
    graph.render();
}

double luminance(const double c[3]) { return 0.2126 * c[0] + 0.7152 * c[1] + 0.0722 * c[2]; }

// A table sampled as a linear, clamp-to-edge sampler does, at (u, v) in [0, 1].
void sample2D(const std::vector<float>& t, int w, int h, int layer, double u, double v, double out[4]) {
    const double fx = std::clamp(u * w - 0.5, 0.0, w - 1.0), fy = std::clamp(v * h - 0.5, 0.0, h - 1.0);
    const int x0 = std::min(static_cast<int>(fx), w - 2), y0 = std::min(static_cast<int>(fy), h - 2);
    const double ax = fx - x0, ay = fy - y0;
    const size_t base = static_cast<size_t>(layer) * w * h;
    for (int c = 0; c < 4; ++c) {
        const auto at = [&](int x, int y) { return static_cast<double>(t[(base + static_cast<size_t>(y) * w + x) * 4 + c]); };
        out[c] = (at(x0, y0) * (1 - ax) + at(x0 + 1, y0) * ax) * (1 - ay) + (at(x0, y0 + 1) * (1 - ax) + at(x0 + 1, y0 + 1) * ax) * ay;
    }
}

struct Stat {
    double worst = 0.0, sum = 0.0;
    int n = 0;
    char where[96] = "";   // the worst sample
    void add(double e, double got = 0.0, double want = 0.0, double a = 0.0, double b = 0.0) {
        if (e > worst) std::snprintf(where, sizeof(where), "at (%.3g, %.3g): %.4g vs %.4g", a, b, got, want);
        worst = std::max(worst, e);
        sum += e;
        ++n;
    }
    double mean() const { return n ? sum / n : 0.0; }
};

void check(const char* what, const Stat& s, double worstTol, double meanTol) {
    const bool ok = s.worst <= worstTol && s.mean() <= meanTol;
    std::printf("    %-34s worst %6.2f%%  mean %6.2f%%  (tolerance %.0f%% / %.0f%%) %s  worst %s\n", what,
                s.worst * 100.0, s.mean() * 100.0, worstTol * 100.0, meanTol * 100.0, ok ? "ok" : "FAILED", s.where);
    if (!ok) ++failures;
}

V3 v3(const float* f) { return {f[0], f[1], f[2]}; }
V3 add(const V3& a, const V3& b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
V3 mul(const V3& a, double s) { return {a.x * s, a.y * s, a.z * s}; }
double dot(const V3& a, const V3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
V3 normalize(const V3& a) { return mul(a, 1.0 / std::sqrt(dot(a, a))); }

}  // namespace

// The tolerances of an air (the file's header says why).
struct Tolerance {
    double dayWorst, dayMean;   // in-scattered light by day (twilight's are 30% / 15% in every air)
    double transmittance;
};

// Every check, for Earth's air with its Mie haze times `turbidity` (the
// AtmosphereConfig option), the eye at `altitudes` and the sun at
// `sunElevations` (degrees).
void checkAir(double turbidity, std::initializer_list<double> altitudes, std::initializer_list<double> sunElevations,
              const Tolerance& tol) {
    const scene::AtmosphereParams defaults;
    scene::atmref::Params rp;
    rp.Rb = defaults.planetRadius;
    rp.Rt = defaults.planetRadius + defaults.thickness;
    rp.mieS = defaults.mieScattering * turbidity;
    rp.mieA = defaults.mieAbsorption * turbidity;
    const double Rb = rp.Rb, Rt = rp.Rt;

    std::printf("air of turbidity %.0f\n", turbidity);
    const auto t0 = std::chrono::steady_clock::now();
    scene::atmref::Reference ref(rp);
    ref.buildMultiple();
    std::printf("reference: %d scattering orders, %.1f s\n", ref.orders(),
                std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count());

    bool checkedTransmittance = false;

    for (const double sunDeg : sunElevations) {
        Stat skyIrr;
        for (const double alt : altitudes) {
            std::printf("  eye %.0f m, sun %.0f deg\n", alt, sunDeg);
            const bool twilight = sunDeg < 0.0;
            scene::SceneGraph graph;
            graph.setCanvasSize(256, 144);
            // Look out toward the sun's azimuth (-z), down to the horizon.
            const double r = Rb + alt;
            const double dip = std::acos(std::min(Rb / r, 1.0));
            const double far = std::max(2000.0, 2.0 * std::sqrt(r * r - Rb * Rb));
            const bromath::Vec3 eye{0.0f, static_cast<float>(alt), 0.0f};
            const bromath::Vec3 target{0.0f, static_cast<float>(alt - std::sin(dip) * 1000.0),
                                       static_cast<float>(-std::cos(dip) * 1000.0)};
            graph.setCamera(static_cast<float>(60.0 * kPi / 180.0), 256.0f / 144.0f, 0.5f, static_cast<float>(far), eye, target);
            scene::AtmosphereParams a;
            a.enabled = true;
            const double se = sunDeg * kPi / 180.0;
            a.sunDir[0] = 0.0f;
            a.sunDir[1] = static_cast<float>(std::sin(se));
            a.sunDir[2] = static_cast<float>(-std::cos(se));
            a.sunDirExplicit = true;
            a.sunColor[0] = a.sunColor[1] = a.sunColor[2] = 1.0f;
            a.sunColorExplicit = true;
            a.mieScattering = static_cast<float>(rp.mieS);
            a.mieAbsorption = static_cast<float>(rp.mieA);
            graph.setAtmosphere(a);
            renderFrame(graph);
            renderFrame(graph);   // the view-independent tables are the world service's, granted on the first
            scene::vk::AtmosphereTables tables;
            if (!graph.renderer().readAtmosphereTables(tables)) {
                std::printf("    FAILED: no tables read back\n");
                ++failures;
                continue;
            }
            for (std::vector<float>* t : {&tables.multiScatter, &tables.skyView, &tables.inscatter, &tables.skyLight})
                for (float& f : *t) f = static_cast<float>(f / atm::kTableScale);
            const scene::vk::AtmosphereGpu& b = tables.block;
            const V3 sun = v3(b.sunDir);

            // --- Transmittance (the table is the same for every case) ---------
            if (!checkedTransmittance) {
                checkedTransmittance = true;
                Stat st;
                for (const double h : {0.0, 500.0, 2000.0, 10000.0, 30000.0, 80000.0}) {
                    const double rr = Rb + h + 1.0;
                    const double muH = -std::sqrt(std::max(1.0 - (Rb / rr) * (Rb / rr), 0.0));
                    for (const double z : {0.0, 30.0, 60.0, 80.0, 86.0, 88.5}) {
                        const double mu = std::cos(z * kPi / 180.0);
                        if (mu < muH + 0.0175) continue;
                        double u, v, s[4];
                        atm::transmittanceUv(Rb, Rt, rr, mu, u, v);
                        sample2D(tables.transmittance, atm::kTransmittanceW, atm::kTransmittanceH, 0, u, v, s);
                        const RGB want = ref.transmittanceToTop(rr, mu);
                        for (int c = 0; c < 3; ++c)
                            st.add(std::abs(s[c] - want.c[c]) / std::max(want.c[c], 1e-3), s[c], want.c[c], h, z);
                    }
                }
                check("transmittance", st, tol.transmittance, 0.01);

                // The multiple-scattering table: informative (the tables it
                // feeds are what is checked), so its departures are seen.
                for (const double h : {2.0, 2000.0, 30000.0}) {
                    std::printf("    multiple scattering at %.0f m:", h);
                    for (const double muS : {0.9, 0.17, 0.0, -0.07, -0.2}) {
                        double u, v, s[4];
                        atm::multiScatterUv(Rb, Rt, Rb + h, muS, u, v);
                        sample2D(tables.multiScatter, atm::kMultiScatterSun, atm::kMultiScatterAlt, 0, u, v, s);
                        const RGB want = ref.meanMultiple(Rb + h, muS);
                        std::printf("  mu_s %.2f: %.3g / %.3g", muS, luminance(s), luminance(want.c));
                    }
                    std::printf("\n");
                }
            }

            // --- Sky radiance from the sky-view table ---------------------------
            {
                const double rv = b.skyView[0], muS = b.skyView[1];
                double zTop, zH;
                atm::skyViewHorizon(Rb, Rt, rv, zTop, zH);
                const V3 ro{0.0, 0.0, rv};
                const V3 sunL{std::sqrt(std::max(1.0 - muS * muS, 0.0)), 0.0, muS};
                struct Sample {
                    double got, want, z, az;   // degrees
                };
                std::vector<Sample> samples;
                // A dense grid: the zenith cap finely, the sky every 2% of its
                // span, under the horizon every 10%; azimuths every 5 degrees,
                // finely about the sun.
                std::vector<double> fs = {0.0, 0.002, 0.005, 0.01};
                for (int i = 1; i <= 50; ++i) fs.push_back(i * 0.02);
                for (int i = 1; i <= 9; ++i) fs.push_back(1.0 + i * 0.1);
                std::vector<double> azs = {0.0, 1.0, 2.0, 3.0};
                for (int i = 1; i <= 36; ++i) azs.push_back(i * 5.0);
                for (const double f : fs) {
                    const double z = f <= 1.0 ? zTop + f * (zH - zTop) : zH + (f - 1.0) * (kPi - zH);
                    if (std::abs(z - zH) < kHorizonBand) continue;
                    for (const double azDeg : azs) {
                        const double az = azDeg * kPi / 180.0;
                        double u, v, s[4];
                        atm::skyViewUv(Rb, Rt, rv, std::cos(z), std::cos(az), u, v);
                        sample2D(tables.skyView, atm::kSkyViewW, atm::kSkyViewH, 0, u, v, s);
                        const V3 rd{std::sin(z) * std::cos(az), std::sin(z) * std::sin(az), std::cos(z)};
                        double ta, tb;
                        bool ground;
                        RGB want;
                        if (ref.segment(ro, rd, ta, tb, ground)) want = ref.radiance(ro, rd, sunL, ta, tb, 1000, nullptr);
                        samples.push_back({luminance(s), luminance(want.c), z * 180.0 / kPi, azDeg});
                    }
                }
                double peak = 0.0;
                for (const Sample& s : samples) peak = std::max(peak, s.want);
                Stat st;
                for (const Sample& s : samples)
                    st.add(std::abs(s.got - s.want) / std::max(s.want, 1e-3 * peak), s.got, s.want, s.z, s.az);
                check("sky radiance (sky-view)", st, twilight ? 0.30 : tol.dayWorst, twilight ? 0.15 : tol.dayMean);
            }

            // --- The sky as drawn ------------------------------------------------
            // The sky pass's pixels (its own direction -> table lookup, the
            // sun's frame and both sides of its azimuth, the dither), every
            // fourth pixel of three views: at the sun (its Mie lobe and the
            // azimuths either side), straight up (the zenith cap) and away
            // from the sun near the horizon. Linear tone curve, a fixed
            // exposure that puts the view's brightest sky at 0.85.
            if (alt < 100000.0) {
                Stat st;
                double zTopUnused, zHorizon;
                atm::skyViewHorizon(Rb, Rt, Rb + alt, zTopUnused, zHorizon);
                const V3 roRef{0.0, 0.0, Rb + alt};
                const V3 sunRef{0.0, std::cos(se), std::sin(se)};   // world (x, y, z) -> (x, -z, y)
                struct View {
                    V3 dir;      // world
                    double fov;  // degrees, vertical
                };
                const View views[] = {{{0.0, std::sin(se), -std::cos(se)}, 90.0},   // at the sun
                                      {{0.0, 1.0, -0.002}, 100.0},                   // straight up
                                      {{0.0, 0.25, 1.0}, 90.0}};                     // away
                for (const View& vw : views) {
                    constexpr int W = 192, H = 192, kStep = 4;
                    scene::SceneGraph g2;
                    g2.setCanvasSize(W, H);
                    const V3 f = normalize(vw.dir);
                    const bromath::Vec3 eye2{0.0f, static_cast<float>(alt), 0.0f};
                    const bromath::Vec3 tgt{static_cast<float>(f.x * 100.0), static_cast<float>(alt + f.y * 100.0),
                                            static_cast<float>(f.z * 100.0)};
                    g2.setCamera(static_cast<float>(vw.fov * kPi / 180.0), 1.0f, 0.5f, 1.0e6f, eye2, tgt);
                    g2.setAtmosphere(a);
                    g2.setToneMap(scene::SceneRenderer::ToneMap::Linear, 1.0f);
                    // The camera's basis (up +y).
                    V3 rgt = normalize({-f.z, 0.0, f.x});   // f x up
                    const V3 up{rgt.y * f.z - rgt.z * f.y, rgt.z * f.x - rgt.x * f.z, rgt.x * f.y - rgt.y * f.x};
                    const double th = std::tan(vw.fov * kPi / 360.0);
                    struct Px {
                        int x, y;
                        double want, spread;   // the reference; its spread over the dither's half texel
                    };
                    std::vector<Px> pxs;
                    double peak = 0.0;
                    for (int y = kStep / 2; y < H; y += kStep)
                        for (int x = kStep / 2; x < W; x += kStep) {
                            const double nx = (x + 0.5) / W * 2.0 - 1.0, ny = 1.0 - (y + 0.5) / H * 2.0;
                            const V3 d = normalize(add(add(f, mul(rgt, nx * th)), mul(up, ny * th)));
                            const double toSun = std::acos(std::clamp(d.y * std::sin(se) - d.z * std::cos(se), -1.0, 1.0));
                            if (toSun < 2.0 * kPi / 180.0) continue;   // the disc and the texels about it
                            const V3 rd{d.x, -d.z, d.y};
                            if (std::abs(std::acos(std::clamp(d.y, -1.0, 1.0)) - zHorizon) < kHorizonBand) continue;
                            double ta, tb;
                            bool ground;
                            if (!ref.segment(roRef, rd, ta, tb, ground) || ground) continue;
                            const RGB want = ref.radiance(roRef, rd, sunRef, ta, tb, 300, nullptr);
                            // The shader dithers its lookup by half a texel
                            // either way: what the reference gives at the
                            // zenith angles half a texel up and down is the
                            // spread a pixel may read (on the limb from high
                            // up, where the light doubles in a degree, it is
                            // what is left).
                            double spread = 0.0;
                            {
                                const double hl = std::hypot(d.x, d.z);
                                double u, v;
                                atm::skyViewUv(Rb, Rt, Rb + alt, d.y, 1.0, u, v);
                                for (const double dv : {-0.5, 0.5}) {
                                    double cz, caz;
                                    atm::skyViewDirection(Rb, Rt, Rb + alt, u, v + dv / atm::kSkyViewH, cz, caz);
                                    const double sz = std::sqrt(std::max(1.0 - cz * cz, 0.0));
                                    const V3 dj = hl > 1e-6 ? V3{d.x / hl * sz, -d.z / hl * sz, cz} : V3{0.0, 0.0, cz};
                                    double ta2, tb2;
                                    bool ground2;
                                    if (!ref.segment(roRef, dj, ta2, tb2, ground2) || ground2) continue;
                                    const RGB w2 = ref.radiance(roRef, dj, sunRef, ta2, tb2, 300, nullptr);
                                    spread = std::max(spread, std::abs(luminance(w2.c) - luminance(want.c)));
                                }
                            }
                            pxs.push_back({x, y, luminance(want.c), spread});
                            peak = std::max(peak, std::max({want.c[0], want.c[1], want.c[2]}));
                        }
                    if (pxs.empty() || !(peak > 0.0)) continue;
                    const double scale = 0.85 / peak;
                    scene::ExposureSettings e;
                    e.ev100 = static_cast<float>(std::log2(1.0 / (1.2 * scale)));
                    g2.setExposure(e);
                    renderFrame(g2);
                    renderFrame(g2);
                    int w = 0, h = 0;
                    const std::vector<uint8_t> px = g2.readTonemapPixelsRGBA(w, h);
                    if (w != W || h != H) {
                        std::printf("    FAILED: no sky pixels read back\n");
                        ++failures;
                        continue;
                    }
                    for (const Px& p : pxs) {
                        const uint8_t* c = &px[(static_cast<size_t>(p.y) * W + p.x) * 4];
                        const double got[3] = {c[0] / 255.0, c[1] / 255.0, c[2] / 255.0};
                        const double want = p.want * scale;
                        // The 8-bit output's step and the dither (1.5 codes)
                        // are not the model's error; a floor of 2% of the
                        // view's peak.
                        const double err = std::max(std::abs(luminance(got) - want) - 1.5 / 255.0 - p.spread * scale, 0.0);
                        // Where: the pixel, the view's index in the thousands of x.
                        st.add(err / std::max(want, 0.02 * 0.85), luminance(got), want,
                               p.x + 1000.0 * static_cast<double>(&vw - views), p.y);
                    }
                }
                check("the sky as drawn (sun, zenith, away)", st, twilight ? 0.30 : tol.dayWorst, twilight ? 0.15 : tol.dayMean);
            }

            // --- Aerial perspective from the froxels ---------------------------
            {
                const V3 ro = v3(b.origin);
                const double depth = b.froxel[3];
                const float* m = tables.froxelInvViewProj;   // column-major
                Stat sl, stt;
                double peak = 0.0;
                struct Sample {
                    double got, want, column, slice;
                };
                std::vector<Sample> ls;
                for (const int cx : {2, 16, 29})
                    for (const int cy : {2, 16, 29}) {
                        const double nx = (cx + 0.5) / atm::kFroxelXY * 2.0 - 1.0;
                        const double ny = (cy + 0.5) / atm::kFroxelXY * 2.0 - 1.0;
                        double hv[4];
                        for (int row = 0; row < 4; ++row)
                            hv[row] = m[0 * 4 + row] * nx + m[1 * 4 + row] * ny + m[2 * 4 + row] * 0.5 + m[3 * 4 + row];
                        const V3 rd = normalize({hv[0] / hv[3], hv[1] / hv[3], hv[2] / hv[3]});
                        double ta, tb;
                        bool ground;
                        const bool air = ref.segment(ro, rd, ta, tb, ground);
                        for (const int k : {0, 3, 10, 20, 31}) {
                            const double d = atm::froxelSliceDistance(depth, k);
                            const size_t i = ((static_cast<size_t>(k) * atm::kFroxelXY + cy) * atm::kFroxelXY + cx) * 4;
                            const double got[3] = {tables.inscatter[i], tables.inscatter[i + 1], tables.inscatter[i + 2]};
                            const double gotT[3] = {tables.airTransmittance[i], tables.airTransmittance[i + 1],
                                                    tables.airTransmittance[i + 2]};
                            RGB want, T;
                            T.c[0] = T.c[1] = T.c[2] = 1.0;
                            if (air && std::min(d, tb) > ta) want = ref.radiance(ro, rd, sun, ta, std::min(d, tb), 2000, &T);
                            ls.push_back({luminance(got), luminance(want.c), cy * 32.0 + cx, double(k)});
                            peak = std::max(peak, luminance(want.c));
                            for (int c = 0; c < 3; ++c)
                                stt.add(std::abs(gotT[c] - T.c[c]) / std::max(T.c[c], 1e-2), gotT[c], T.c[c], cy * 32.0 + cx, k);
                        }
                    }
                for (const Sample& s : ls)
                    sl.add(std::abs(s.got - s.want) / std::max(s.want, 1e-3 * peak + 1e-12), s.got, s.want, s.column, s.slice);
                check("aerial perspective in-scatter", sl, twilight ? 0.30 : tol.dayWorst, twilight ? 0.15 : tol.dayMean);
                check("aerial perspective transmittance", stt, 0.02, 0.01);
            }

            // --- The sky's light on an upward surface at the ground -------------
            if (alt == 2.0) {
                double u, v;
                atm::skyLightUv(defaults.thickness, 1.0, b.skyView[1], u, v);
                double layer[5][4];
                for (int l = 0; l < 5; ++l)
                    sample2D(tables.skyLight, atm::kSkyLightSun, atm::kSkyLightAlt, l, u, v, layer[l]);
                const double* f = &layer[0][0];   // 20 floats, the first 18 the coefficients
                double got[3];
                for (int c = 0; c < 3; ++c) {
                    double sh[6];
                    for (int j = 0; j < 6; ++j) sh[j] = f[3 * j + c];
                    got[c] = atm::shIrradianceOverPi(sh, 0.0, 0.0, 1.0);
                }
                const RGB want = ref.groundSkyIrradianceOverPi(b.skyView[1]);
                skyIrr.add(std::abs(luminance(got) - luminance(want.c)) / std::max(luminance(want.c), 1e-9));
                std::printf("    sky irradiance / pi (luminance): table %.5g, reference %.5g\n", luminance(got),
                            luminance(want.c));
            }
        }
        check("sky light on the ground (SH)", skyIrr, 0.15, 0.15);
    }
}

int main() {
    std::printf("=== bro_atmosphere_test: the atmosphere's tables against a CPU reference ===\n");

    render::VulkanContextConfig cfg;
    cfg.headless = true;
    cfg.enableValidation = false;
    cfg.enableDynamicRendering = true;
    render::VulkanContext context(cfg);
    if (!context.init()) {
        std::fprintf(stderr, "Failed to initialize headless Vulkan context!\n");
        return 1;
    }
    scene::SceneRenderer::setDefaultVulkanContext(&context);

    checkAir(1.0, {2.0, 2000.0, 30000.0, 400000.0}, {68.0, 10.0, -4.0}, {0.12, 0.08, 0.01});
    // A clear day's haze (turbidity 10, about 65 km of visibility: the
    // workshop kit's air), near the ground, where its Mie lobe about the sun
    // and the haze toward the horizon are most of the sky.
    checkAir(10.0, {2.0, 2000.0}, {68.0, 35.0, 10.0}, {0.16, 0.09, 0.015});

    scene::SceneRenderer::setDefaultVulkanContext(nullptr);
    if (const uint32_t errors = render::vulkanValidationErrorCount()) {
        std::fprintf(stderr, "FAILED: %u Vulkan validation error(s)\n", errors);
        return 1;
    }
    if (failures) {
        std::printf("=== bro_atmosphere_test: %d check(s) FAILED ===\n", failures);
        return 1;
    }
    std::printf("=== bro_atmosphere_test: all checks passed ===\n");
    return 0;
}
