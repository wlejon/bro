#include "scene/world/cover_regions.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace bro::scene {

namespace {

constexpr double kPi = 3.14159265358979323846;
// Points a cell's edge is sampled at for its extent in its flat frame.
constexpr int kEdgeSamples = 16;

}  // namespace

void CoverRegion::toLand(const Place& place, double x, double z, double h, double out[3]) const {
    if (flat) {
        out[0] = x;
        out[1] = h;
        out[2] = z;
        return;
    }
    double p[3];
    place.mapToLand(x, z, h, p);
    for (int i = 0; i < 3; ++i) out[i] = rot[i * 3] * p[0] + rot[i * 3 + 1] * p[1] + rot[i * 3 + 2] * p[2] + origin[i];
}

void CoverRegion::landMapBox(const Place& place, double x0, double z0, double x1, double z1, double out[4]) const {
    if (flat) {
        out[0] = x0;
        out[1] = z0;
        out[2] = x1;
        out[3] = z1;
        return;
    }
    out[0] = out[1] = std::numeric_limits<double>::max();
    out[2] = out[3] = -std::numeric_limits<double>::max();
    for (int k = 0; k < 9; ++k) {
        const double x = k % 3 == 0 ? x0 : k % 3 == 1 ? 0.5 * (x0 + x1) : x1;
        const double z = k / 3 == 0 ? z0 : k / 3 == 1 ? 0.5 * (z0 + z1) : z1;
        double p[3], mx, mz, h;
        toLand(place, x, z, 0.0, p);
        place.landToMap(p, mx, mz, h);
        out[0] = std::min(out[0], mx);
        out[1] = std::min(out[1], mz);
        out[2] = std::max(out[2], mx);
        out[3] = std::max(out[3], mz);
    }
}

bromath::AABB3 CoverRegion::landBox(const Place& place, float x0, float z0, float x1, float z1, float lo,
                                    float hi) const {
    if (flat) return {{x0, lo, z0}, {x1, hi, z1}};
    bromath::AABB3 b = bromath::aempty3();
    for (int k = 0; k < 8; ++k) {
        double p[3];
        toLand(place, k & 1 ? x1 : x0, k & 2 ? z1 : z0, k & 4 ? hi : lo, p);
        const bromath::Vec3 v{static_cast<float>(p[0]), static_cast<float>(p[1]), static_cast<float>(p[2])};
        b.min = bromath::vmin(b.min, v);
        b.max = bromath::vmax(b.max, v);
    }
    // The ground between the corners stands above them (a chord of the
    // body's curve), by as much as its span squared over 8R.
    const double s2 = static_cast<double>(x1 - x0) * (x1 - x0) + static_cast<double>(z1 - z0) * (z1 - z0);
    const float sag = static_cast<float>(s2 / (8.0 * place.body().radius)) + 0.01f;
    b.min = b.min - bromath::Vec3{sag, sag, sag};
    b.max = b.max + bromath::Vec3{sag, sag, sag};
    return b;
}

bromath::Vec3 CoverRegion::fromLand(const bromath::Vec3& d) const {
    if (flat) return d;
    return {static_cast<float>(rot[0] * d.x + rot[3] * d.y + rot[6] * d.z),
            static_cast<float>(rot[1] * d.x + rot[4] * d.y + rot[7] * d.z),
            static_cast<float>(rot[2] * d.x + rot[5] * d.y + rot[8] * d.z)};
}

void coverRegions(const Place& place, const bromath::Vec3& eye, double reach, double band,
                  std::vector<CoverRegion>& out) {
    out.clear();
    const Body& body = place.body();
    if (body.flat()) {
        CoverRegion r;
        r.eye = eye;
        r.reach = std::numeric_limits<float>::infinity();
        out.push_back(r);
        return;
    }
    const double R = body.radius;
    const double* OA = place.origin();
    const double* BA = place.basis();
    double eyeBody[3];
    place.toBody(eye, eyeBody);
    double em[3] = {eye.x, eye.y, eye.z}, emx, emz, eh;
    place.landToMap(em, emx, emz, eh);

    // The cells under a grid over the square of `reach` about the eye: a
    // cell is over 11 km of ground a side, so none reaching into the square
    // misses every point of it.
    std::vector<Anchor> cells;
    for (int j = -2; j <= 2; ++j)
        for (int i = -2; i <= 2; ++i) {
            double p[3], b[3];
            place.mapToLand(emx + 0.5 * reach * i, emz + 0.5 * reach * j, 0.0, p);
            place.toBody(p, b);
            const Anchor a = place.anchorOf(b);
            if (std::find(cells.begin(), cells.end(), a) == cells.end()) cells.push_back(a);
        }

    const double step = 0.5 * kPi / place.faceCells();
    double dirA[3] = {OA[0] / R, OA[1] / R, OA[2] / R};
    for (const Anchor& a : cells) {
        CoverRegion r;
        r.cell = a;
        r.flat = false;
        double OR[3], BR[9];
        place.frameOf(a, OR, BR);
        for (int i = 0; i < 3; ++i) {
            for (int j = 0; j < 3; ++j) {
                double s = 0.0;
                for (int k = 0; k < 3; ++k) s += BA[k * 3 + i] * BR[k * 3 + j];
                r.rot[i * 3 + j] = s;
            }
            double s = 0.0;
            for (int k = 0; k < 3; ++k) s += BA[k * 3 + i] * (OR[k] - OA[k]);
            r.origin[i] = s;
        }
        // A body point in the region's frame, then its flat point.
        const auto flatOf = [&](const double b[3], double& x, double& z, double& h) {
            double p[3];
            for (int j = 0; j < 3; ++j) p[j] = BR[j] * (b[0] - OR[0]) + BR[3 + j] * (b[1] - OR[1]) + BR[6 + j] * (b[2] - OR[2]);
            place.landToMap(p, x, z, h);
        };
        double ex, ez, ehR;
        flatOf(eyeBody, ex, ez, ehR);
        r.eye = {static_cast<float>(ex), static_cast<float>(ehR), static_cast<float>(ez)};
        for (int i = 0; i < 3; ++i) {
            for (int j = 0; j < 3; ++j) r.frame[i * 4 + j] = static_cast<float>(r.rot[i * 3 + j]);
            r.frame[i * 4 + 3] = static_cast<float>(r.origin[i]);
        }
        r.frame[12] = r.eye.x;
        r.frame[13] = r.eye.y;
        r.frame[14] = r.eye.z;
        r.frame[15] = 1.0f;

        // Its chart cell, from the anchor's point on its face.
        const ChartPoint cA = chartOf(body, dirA, a.face);
        const double x0 = (-0.25 * kPi + a.x * step) * R, z0 = (-0.25 * kPi + a.z * step) * R;
        const double x1 = x0 + step * R, z1 = z0 + step * R;
        r.own[0] = static_cast<float>(x0 - cA.x);
        r.own[1] = static_cast<float>(z0 - cA.z);
        r.own[2] = static_cast<float>(x1 - cA.x);
        r.own[3] = static_cast<float>(z1 - cA.z);
        // Its extent in its flat frame, from its edges.
        double inner = std::numeric_limits<double>::max(), outer = 0.0;
        for (int e = 0; e < 4; ++e)
            for (int k = 0; k <= kEdgeSamples; ++k) {
                const double t = static_cast<double>(k) / kEdgeSamples;
                ChartPoint c{a.face, 0.0, 0.0};
                c.x = e == 0 ? x0 : e == 1 ? x1 : x0 + t * (x1 - x0);
                c.z = e == 2 ? z0 : e == 3 ? z1 : z0 + t * (z1 - z0);
                double d[3], b[3], x, z, h;
                dirOf(body, c, d);
                for (int i = 0; i < 3; ++i) b[i] = R * d[i];
                flatOf(b, x, z, h);
                const double m = std::max(std::fabs(x), std::fabs(z));
                inner = std::min(inner, m);
                outer = std::max(outer, m);
            }
        r.own2[0] = static_cast<float>(a.face);
        r.own2[1] = static_cast<float>(band);
        r.own2[2] = static_cast<float>(inner);
        r.reach = static_cast<float>(outer + band);
        out.push_back(r);
    }
}

}  // namespace bro::scene
