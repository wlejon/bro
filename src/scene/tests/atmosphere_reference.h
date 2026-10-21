#pragma once

// A CPU reference for the atmosphere (bro_atmosphere_test): the same media
// as scene/world/atmosphere.h, integrated by brute force in double, with
// none of the tables' shortcuts.
//
//  - Transmittance: a midpoint march of the optical depth (transmittance()),
//    and a 256 x 256 table of 1000-step marches for the sun's light at any
//    point (sunTransmittance(), the planet's shadow the share of the sun's
//    disc above the horizon).
//  - Single scattering: a 4000-step march of the view ray with the true
//    phase functions, the sun's light at every step from that table.
//  - Multiple scattering: the orders iterated. Order n's radiance field at
//    every point of a (radius, sun angle) grid is gathered over 256
//    directions on the sphere, each a 64-step march spaced quadratically
//    from the point; order n + 1 scatters
//    the mean of order n's radiance isotropically (an approximation of the
//    phase only for the second scattering on, where the light is smooth)
//    and the ground reflects its irradiance (Lambertian). The orders stop
//    when the next one adds under 0.1% of the sum so far.
// So the reference differs from the GPU's model in exactly what Hillaire's
// tables approximate: the multiple scattering's form (one isotropic
// geometric series from the surroundings of each point, L2 / (1 - f_ms)),
// the tables' resolution and their half floats.

#include <cstdint>
#include <vector>

namespace bro::scene::atmref {

struct V3 {
    double x = 0.0, y = 0.0, z = 0.0;
};

struct Params {
    double Rb = 6360000.0, Rt = 6460000.0;
    double rayleigh[3] = {5.802e-6, 13.558e-6, 33.1e-6};
    double HR = 8000.0;
    double mieS = 3.996e-6, mieA = 0.444e-6, g = 0.8, HM = 1200.0;
    double ozone[3] = {0.650e-6, 1.881e-6, 0.085e-6};
    double ozC = 25000.0, ozW = 15000.0;
    double albedo[3] = {0.3, 0.3, 0.3};
    double sunAlpha = 0.004675;
};

struct RGB {
    double c[3] = {0.0, 0.0, 0.0};
};

class Reference {
public:
    explicit Reference(const Params& p);

    /// The air at radius r: Rayleigh and Mie scattering, total extinction.
    void medium(double r, double sR[3], double& sM, double ext[3]) const;
    /// Transmittance along ro + rd t, t in [0, t1] (midpoint, `steps`).
    RGB transmittance(const V3& ro, const V3& rd, double t1, int steps) const;
    /// From radius r along mu to the top (0 when it meets the ground), marched.
    RGB transmittanceToTop(double r, double mu, int steps = 4000) const;
    /// The sun's light at radius r, sun at mu_s (sun 1): the table's
    /// transmittance times the share of the disc above the horizon.
    RGB sunTransmittance(double r, double muS) const;

    /// Iterate the scattering orders (call once).
    void buildMultiple();
    int orders() const { return orders_; }

    /// The light the air sends to `ro` along `rd` from [t0, t1] (sun 1 toward
    /// `sun`): single scattering by the true phases plus every order of
    /// multiple scattering; `T` the segment's transmittance.
    RGB radiance(const V3& ro, const V3& rd, const V3& sun, double t0, double t1, int steps, RGB* T) const;
    /// The air a ray crosses: false when it misses; `ground` when it ends there.
    bool segment(const V3& ro, const V3& rd, double& t0, double& t1, bool& ground) const;

    /// The sky's irradiance (sun 1) on an upward surface at the ground with
    /// the sun at mu_s, over pi: the whole sky, every order.
    RGB groundSkyIrradianceOverPi(double muS) const;

    /// The mean radiance of all orders 1..orders at a point (sun 1): what
    /// the air there scatters again, isotropically (the GPU's multiple-
    /// scattering table holds its approximation).
    RGB meanMultiple(double r, double muS) const;

    const Params& params() const { return p_; }

private:
    double phaseR(double nu) const;
    double phaseM(double nu) const;
    RGB meanOrder(const std::vector<RGB>& table, double r, double muS) const;
    RGB groundIrradiance(const std::vector<RGB>& table, double muS) const;

    Params p_;
    // The sun's transmittance table, (r, mu) in Bruneton's mapping.
    static constexpr int kTW = 256, kTH = 256;
    std::vector<RGB> tTable_;
    // The multiple-scattering grid: kMR radii (squared spacing) x kMS sun angles.
    static constexpr int kMR = 64, kMS = 256;
    std::vector<RGB> gSum_;   // the sum over orders 1..n of each order's mean radiance
    std::vector<RGB> eSum_;   // the same orders' irradiance on the ground, by mu_s (kMS)
    int orders_ = 0;
};

}  // namespace bro::scene::atmref
