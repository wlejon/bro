#include "atmosphere_reference.h"

#include <algorithm>
#include <cmath>
#include <thread>

namespace bro::scene::atmref {

namespace {

constexpr double kPi = 3.14159265358979323846;

V3 add(const V3& a, const V3& b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
V3 mul(const V3& a, double s) { return {a.x * s, a.y * s, a.z * s}; }
double dot(const V3& a, const V3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
double len(const V3& a) { return std::sqrt(dot(a, a)); }

// Where a ray from `ro` leaves the sphere of radius R; -1 if never.
double sphereFar(const V3& ro, const V3& rd, double R) {
    const double b = dot(ro, rd), c = dot(ro, ro) - R * R, d = b * b - c;
    return d < 0.0 ? -1.0 : -b + std::sqrt(d);
}
double sphereNear(const V3& ro, const V3& rd, double R) {
    const double b = dot(ro, rd), c = dot(ro, ro) - R * R;
    if (c <= 0.0) return 0.0;
    if (b > 0.0) return -1.0;
    const double d = b * b - c;
    return d < 0.0 ? -1.0 : -b - std::sqrt(d);
}
double groundHit(const V3& ro, const V3& rd, double Rb) {
    const double b = dot(ro, rd), c = dot(ro, ro) - Rb * Rb, d = b * b - c;
    if (d < 0.0) return -1.0;
    const double t = -b - std::sqrt(d);
    return t >= 0.0 ? t : -1.0;
}

// The point at radius r with the sun at mu_s in the (x, z) plane, z up.
void frame(double r, double muS, V3& p, V3& sun) {
    p = {0.0, 0.0, r};
    sun = {std::sqrt(std::max(1.0 - muS * muS, 0.0)), 0.0, muS};
}

// N directions spread evenly over the sphere (a Fibonacci lattice).
std::vector<V3> sphereDirections(int n) {
    std::vector<V3> out(n);
    for (int i = 0; i < n; ++i) {
        const double z = 1.0 - 2.0 * (i + 0.5) / n;
        const double phi = 2.39996322972865332 * (i + 0.5);
        const double s = std::sqrt(std::max(1.0 - z * z, 0.0));
        out[i] = {s * std::cos(phi), s * std::sin(phi), z};
    }
    return out;
}

template <class F>
void parallelFor(int n, F&& f) {
    const int threads = std::max(1u, std::thread::hardware_concurrency());
    std::vector<std::thread> pool;
    for (int t = 0; t < threads; ++t)
        pool.emplace_back([&, t] {
            for (int i = t; i < n; i += threads) f(i);
        });
    for (std::thread& th : pool) th.join();
}

// Bruneton's (r, mu) mapping for the reference's own transmittance table.
void tUv(double Rb, double Rt, double r, double mu, double& x, double& y) {
    const double H = std::sqrt(Rt * Rt - Rb * Rb);
    const double rho = std::sqrt(std::max(r * r - Rb * Rb, 0.0));
    const double disc = r * r * (mu * mu - 1.0) + Rt * Rt;
    const double d = std::max(-r * mu + std::sqrt(std::max(disc, 0.0)), 0.0);
    const double dMin = Rt - r, dMax = rho + H;
    x = std::clamp(dMax > dMin ? (d - dMin) / (dMax - dMin) : 0.0, 0.0, 1.0);
    y = std::clamp(rho / H, 0.0, 1.0);
}

}  // namespace

Reference::Reference(const Params& p) : p_(p) {
    // The sun's transmittance at every (r, mu) above the horizon, marched.
    tTable_.resize(static_cast<size_t>(kTW) * kTH);
    const double Rb = p_.Rb, Rt = p_.Rt, H = std::sqrt(Rt * Rt - Rb * Rb);
    parallelFor(kTH, [&](int j) {
        for (int i = 0; i < kTW; ++i) {
            const double x = static_cast<double>(i) / (kTW - 1), y = static_cast<double>(j) / (kTH - 1);
            const double rho = H * y, r = std::sqrt(rho * rho + Rb * Rb);
            const double dMin = Rt - r, dMax = rho + H, d = dMin + x * (dMax - dMin);
            const double mu = d <= 0.0 ? 1.0 : std::clamp((H * H - rho * rho - d * d) / (2.0 * r * d), -1.0, 1.0);
            const V3 ro{0.0, 0.0, r}, rd{std::sqrt(std::max(1.0 - mu * mu, 0.0)), 0.0, mu};
            tTable_[static_cast<size_t>(j) * kTW + i] = transmittance(ro, rd, std::max(sphereFar(ro, rd, Rt), 0.0), 1000);
        }
    });
}

void Reference::medium(double r, double sR[3], double& sM, double ext[3]) const {
    const double h = std::max(r - p_.Rb, 0.0);
    if (r > p_.Rt) {
        sR[0] = sR[1] = sR[2] = sM = ext[0] = ext[1] = ext[2] = 0.0;
        return;
    }
    const double dR = std::exp(-h / p_.HR), dM = std::exp(-h / p_.HM);
    const double dO = std::max(0.0, 1.0 - std::abs(h - p_.ozC) / p_.ozW);
    sM = p_.mieS * dM;
    for (int c = 0; c < 3; ++c) {
        sR[c] = p_.rayleigh[c] * dR;
        ext[c] = sR[c] + (p_.mieS + p_.mieA) * dM + p_.ozone[c] * dO;
    }
}

RGB Reference::transmittance(const V3& ro, const V3& rd, double t1, int steps) const {
    double depth[3] = {0.0, 0.0, 0.0};
    const double dt = t1 / steps;
    for (int i = 0; i < steps; ++i) {
        double sR[3], sM, ext[3];
        medium(len(add(ro, mul(rd, dt * (i + 0.5)))), sR, sM, ext);
        for (int c = 0; c < 3; ++c) depth[c] += ext[c] * dt;
    }
    RGB out;
    for (int c = 0; c < 3; ++c) out.c[c] = std::exp(-depth[c]);
    return out;
}

RGB Reference::transmittanceToTop(double r, double mu, int steps) const {
    const V3 ro{0.0, 0.0, r}, rd{std::sqrt(std::max(1.0 - mu * mu, 0.0)), 0.0, mu};
    if (groundHit(ro, rd, p_.Rb) >= 0.0) return {};
    return transmittance(ro, rd, std::max(sphereFar(ro, rd, p_.Rt), 0.0), steps);
}

RGB Reference::sunTransmittance(double r, double muS) const {
    r = std::clamp(r, p_.Rb, p_.Rt);
    // The share of the sun's disc above the horizon: the disc's area past a
    // chord at the horizon's angle (in units of the disc's radius).
    const double zH = kPi - std::asin(std::min(p_.Rb / r, 1.0));
    const double x = (zH - std::acos(std::clamp(muS, -1.0, 1.0))) / p_.sunAlpha;
    double share = 1.0;
    if (x <= -1.0) return {};
    if (x < 1.0) share = 1.0 - (std::acos(x) - x * std::sqrt(1.0 - x * x)) / kPi;
    // The air to the top, from the table, at the horizon for a sun below it.
    double u, v;
    tUv(p_.Rb, p_.Rt, r, std::max(muS, std::cos(zH)), u, v);
    const double fx = u * (kTW - 1), fy = v * (kTH - 1);
    const int x0 = std::min(static_cast<int>(fx), kTW - 2), y0 = std::min(static_cast<int>(fy), kTH - 2);
    const double ax = fx - x0, ay = fy - y0;
    const auto at = [&](int i, int j) { return tTable_[static_cast<size_t>(j) * kTW + i]; };
    RGB out;
    for (int c = 0; c < 3; ++c) {
        const double a = at(x0, y0).c[c] * (1 - ax) + at(x0 + 1, y0).c[c] * ax;
        const double b = at(x0, y0 + 1).c[c] * (1 - ax) + at(x0 + 1, y0 + 1).c[c] * ax;
        out.c[c] = share * (a * (1 - ay) + b * ay);
    }
    return out;
}

double Reference::phaseR(double nu) const { return 3.0 / (16.0 * kPi) * (1.0 + nu * nu); }
double Reference::phaseM(double nu) const {
    const double g = p_.g, g2 = g * g;
    const double d = std::max(1.0 + g2 - 2.0 * g * nu, 1e-8);
    return 3.0 / (8.0 * kPi) * ((1.0 - g2) * (1.0 + nu * nu)) / ((2.0 + g2) * d * std::sqrt(d));
}

namespace {
// The grid's radius for row i (squared spacing, fine near the ground) and back.
double gridR(const Params& p, int i, int n) {
    const double s = static_cast<double>(i) / (n - 1);
    return p.Rb + 1.0 + s * s * (p.Rt - p.Rb - 2.0);
}
double gridRow(const Params& p, double r, int n) {
    const double s = std::sqrt(std::clamp((r - p.Rb - 1.0) / (p.Rt - p.Rb - 2.0), 0.0, 1.0));
    return s * (n - 1);
}
}  // namespace

RGB Reference::meanOrder(const std::vector<RGB>& table, double r, double muS) const {
    const double fy = gridRow(p_, r, kMR);
    const double fx = std::clamp(0.5 + 0.5 * muS, 0.0, 1.0) * (kMS - 1);
    const int x0 = std::min(static_cast<int>(fx), kMS - 2), y0 = std::min(static_cast<int>(fy), kMR - 2);
    const double ax = fx - x0, ay = fy - y0;
    const auto at = [&](int i, int j) { return table[static_cast<size_t>(j) * kMS + i]; };
    RGB out;
    for (int c = 0; c < 3; ++c) {
        const double a = at(x0, y0).c[c] * (1 - ax) + at(x0 + 1, y0).c[c] * ax;
        const double b = at(x0, y0 + 1).c[c] * (1 - ax) + at(x0 + 1, y0 + 1).c[c] * ax;
        out.c[c] = a * (1 - ay) + b * ay;
    }
    return out;
}

RGB Reference::meanMultiple(double r, double muS) const { return meanOrder(gSum_, r, muS); }

RGB Reference::groundIrradiance(const std::vector<RGB>& table, double muS) const {
    const double fx = std::clamp(0.5 + 0.5 * muS, 0.0, 1.0) * (kMS - 1);
    const int x0 = std::min(static_cast<int>(fx), kMS - 2);
    const double ax = fx - x0;
    RGB out;
    for (int c = 0; c < 3; ++c) out.c[c] = table[x0].c[c] * (1 - ax) + table[x0 + 1].c[c] * ax;
    return out;
}

void Reference::buildMultiple() {
    // Order n's mean radiance G_n(r, mu_s) over the sphere at each grid
    // point, and E_n(mu_s), order n's irradiance on the ground. Order 1 is
    // the sun scattered once by the true phases, plus the sun reflected once
    // by the ground; order n + 1 is order n's mean scattered isotropically,
    // plus order n's irradiance reflected by the ground. Each direction is
    // marched in kSteps segments spaced quadratically from the point (the
    // air thins away from it along most rays), with the transmittance along.
    constexpr int kDirs = 256, kSteps = 64;
    const std::vector<V3> dirs = sphereDirections(kDirs);
    const size_t cells = static_cast<size_t>(kMR) * kMS;
    std::vector<RGB> prevG, prevE;   // order n - 1 (empty for order 1)
    gSum_.assign(cells, RGB{});
    eSum_.assign(kMS, RGB{});

    // The radiance of order n arriving at `ro` along `rd` (sun towards `sun`).
    const auto arriving = [&](const V3& ro, const V3& rd, const V3& sun, int order) {
        RGB L;
        double t1 = sphereFar(ro, rd, p_.Rt);
        const double g = groundHit(ro, rd, p_.Rb);
        const bool ground = g >= 0.0 && g < t1;
        if (ground) t1 = g;
        if (t1 <= 0.0) return L;
        const double nu = dot(rd, sun);
        const double pR = phaseR(nu), pM = phaseM(nu);
        double T[3] = {1.0, 1.0, 1.0};
        for (int s = 0; s < kSteps; ++s) {
            const double a = static_cast<double>(s) / kSteps, b = static_cast<double>(s + 1) / kSteps;
            const double ta = t1 * a * a, tb = t1 * b * b, dt = tb - ta;
            const V3 q = add(ro, mul(rd, 0.5 * (ta + tb)));
            const double rq = len(q), muq = dot(q, sun) / rq;
            double sR[3], sM, ext[3];
            medium(rq, sR, sM, ext);
            RGB src;
            if (order == 1) {
                const RGB sT = sunTransmittance(rq, muq);
                for (int c = 0; c < 3; ++c) src.c[c] = (sR[c] * pR + sM * pM) * sT.c[c];
            } else {
                const RGB G = meanOrder(prevG, rq, muq);
                for (int c = 0; c < 3; ++c) src.c[c] = (sR[c] + sM) * G.c[c];
            }
            for (int c = 0; c < 3; ++c) {
                const double st = std::exp(-ext[c] * dt);
                const double w = ext[c] > 1e-14 ? (1.0 - st) / ext[c] : dt;
                L.c[c] += T[c] * src.c[c] * w;
                T[c] *= st;
            }
        }
        if (ground) {
            const V3 q = add(ro, mul(rd, t1));
            const double mu = dot(q, sun) / len(q);
            RGB e;
            if (order == 1) {
                const RGB sT = sunTransmittance(p_.Rb, mu);
                for (int c = 0; c < 3; ++c) e.c[c] = sT.c[c] * std::max(mu, 0.0);
            } else {
                e = groundIrradiance(prevE, mu);
            }
            for (int c = 0; c < 3; ++c) L.c[c] += T[c] * p_.albedo[c] / kPi * e.c[c];
        }
        return L;
    };

    std::vector<RGB> G(cells), E(kMS);
    for (int order = 1; order <= 40; ++order) {
        parallelFor(static_cast<int>(cells), [&](int idx) {
            const int i = idx % kMS, j = idx / kMS;
            V3 ro, sun;
            frame(gridR(p_, j, kMR), -1.0 + 2.0 * i / (kMS - 1), ro, sun);
            RGB sum;
            for (const V3& rd : dirs) {
                const RGB L = arriving(ro, rd, sun, order);
                for (int c = 0; c < 3; ++c) sum.c[c] += L.c[c];
            }
            for (int c = 0; c < 3; ++c) sum.c[c] /= kDirs;
            G[idx] = sum;
        });
        parallelFor(kMS, [&](int i) {
            V3 ro, sun;
            frame(p_.Rb + 1.0, -1.0 + 2.0 * i / (kMS - 1), ro, sun);
            RGB sum;
            for (const V3& rd : dirs) {
                if (rd.z <= 0.0) continue;
                const RGB L = arriving(ro, rd, sun, order);
                for (int c = 0; c < 3; ++c) sum.c[c] += L.c[c] * rd.z;
            }
            for (int c = 0; c < 3; ++c) sum.c[c] *= 4.0 * kPi / kDirs;
            E[i] = sum;
        });
        double added = 0.0, total = 0.0;
        for (size_t k = 0; k < cells; ++k)
            for (int c = 0; c < 3; ++c) {
                gSum_[k].c[c] += G[k].c[c];
                added += G[k].c[c];
                total += gSum_[k].c[c];
            }
        for (int i = 0; i < kMS; ++i)
            for (int c = 0; c < 3; ++c) eSum_[i].c[c] += E[i].c[c];
        prevG = G;
        prevE = E;
        orders_ = order;
        if (order > 1 && added < 1e-3 * total) break;
    }
}

bool Reference::segment(const V3& ro, const V3& rd, double& t0, double& t1, bool& ground) const {
    t0 = sphereNear(ro, rd, p_.Rt);
    t1 = sphereFar(ro, rd, p_.Rt);
    ground = false;
    if (t0 < 0.0 || t1 <= t0) return false;
    const double g = groundHit(ro, rd, p_.Rb);
    if (g >= 0.0 && g < t1) {
        t1 = g;
        ground = true;
    }
    return t1 > t0;
}

RGB Reference::radiance(const V3& ro, const V3& rd, const V3& sun, double t0, double t1, int steps, RGB* Tout) const {
    RGB L;
    double T[3] = {1.0, 1.0, 1.0};
    const double nu = dot(rd, sun);
    const double pR = phaseR(nu), pM = phaseM(nu);
    const double dt = (t1 - t0) / steps;
    for (int s = 0; s < steps && dt > 0.0; ++s) {
        const V3 q = add(ro, mul(rd, t0 + dt * (s + 0.5)));
        const double rq = len(q), muq = dot(q, sun) / rq;
        double sR[3], sM, ext[3];
        medium(rq, sR, sM, ext);
        const RGB sT = sunTransmittance(rq, muq);
        const RGB G = meanMultiple(rq, muq);
        for (int c = 0; c < 3; ++c) {
            const double src = (sR[c] * pR + sM * pM) * sT.c[c] + (sR[c] + sM) * G.c[c];
            const double st = std::exp(-ext[c] * dt);
            const double w = ext[c] > 1e-14 ? (1.0 - st) / ext[c] : dt;
            L.c[c] += T[c] * src * w;
            T[c] *= st;
        }
    }
    if (Tout)
        for (int c = 0; c < 3; ++c) Tout->c[c] = T[c];
    return L;
}

RGB Reference::groundSkyIrradianceOverPi(double muS) const {
    const std::vector<V3> dirs = sphereDirections(2048);
    V3 ro, sun;
    frame(p_.Rb + 1.0, muS, ro, sun);
    RGB sum;
    for (const V3& rd : dirs) {
        if (rd.z <= 0.0) continue;
        double t0, t1;
        bool ground;
        if (!segment(ro, rd, t0, t1, ground)) continue;
        const RGB L = radiance(ro, rd, sun, t0, t1, 400, nullptr);
        for (int c = 0; c < 3; ++c) sum.c[c] += L.c[c] * rd.z;
    }
    for (int c = 0; c < 3; ++c) sum.c[c] *= 4.0 / dirs.size();   // (4 pi / N) / pi
    return sum;
}

}  // namespace bro::scene::atmref
