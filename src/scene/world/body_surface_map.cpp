#include "scene/world/body_surface_map.h"

#include <algorithm>
#include <cmath>

namespace bro::scene {

namespace {

double dot3(const double a[3], const double b[3]) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }

double angleBetween(const double a[3], const double b[3]) {
    const double c[3] = {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]};
    return std::atan2(std::sqrt(dot3(c, c)), dot3(a, b));
}

}  // namespace

bool beyondLimb(const Body& body, const double eye[3], const double centre[3], double radius, double lowest,
                double highest) {
    const double R = body.radius;
    const double low = R + std::min(lowest, 0.0);
    const double le = std::sqrt(dot3(eye, eye));
    if (!(le > low) || !(R + highest > low)) return false;
    // The eye sees ground at most acos(low / |e|) round the body to the
    // limb, and a point of height H stands over the limb from acos(low / (R
    // + H)) further on.
    const double en[3] = {eye[0] / le, eye[1] / le, eye[2] / le};
    const double nearest = angleBetween(en, centre) - radius;
    const double reach = std::acos(std::min(low / le, 1.0)) + std::acos(std::min(low / (R + highest), 1.0));
    return nearest > reach;
}

SurfaceMap::Patch BodySurfaceMap::patch(int root, double x0, double z0, double x1, double z1, float lo, float hi,
                                        const bromath::Vec3& eye) const {
    Patch out;
    const Body& body = place_->body();
    const double R = body.radius;
    // The square's corners and centre; the box holds them at both heights,
    // widened by how far the ground bulges past a chord between two of them.
    double centre[3];
    dirOf(body, {root, 0.5 * (x0 + x1), 0.5 * (z0 + z1)}, centre);
    double radius = 0.0;   // the square's angular radius about its centre
    bromath::Vec3 mn{1e30f, 1e30f, 1e30f}, mx{-1e30f, -1e30f, -1e30f};
    for (int c = 0; c < 5; ++c) {
        double d[3];
        if (c < 4)
            dirOf(body, {root, (c & 1) ? x1 : x0, (c & 2) ? z1 : z0}, d);
        else
            d[0] = centre[0], d[1] = centre[1], d[2] = centre[2];
        radius = std::max(radius, angleBetween(d, centre));
        for (const float h : {lo, hi}) {
            const double b[3] = {(R + h) * d[0], (R + h) * d[1], (R + h) * d[2]};
            const bromath::Vec3 p = place_->toPlace(b);
            mn = bromath::vmin(mn, p);
            mx = bromath::vmax(mx, p);
        }
    }
    // Two neighbouring corners are at most twice the square's angular
    // radius apart: the ground between them bulges past their chord by at
    // most (R + hi)(1 - cos(radius)), and past the box no further.
    const float bulge = static_cast<float>((R + hi) * (1.0 - std::cos(radius))) + 1.0f;
    out.box = {mn - bromath::Vec3{bulge, bulge, bulge}, mx + bromath::Vec3{bulge, bulge, bulge}};

    // The eye on the body, its height over the square's tangent plane at
    // the middle of its heights.
    double e[3];
    place_->toBody(eye, e);
    const double mid = R + 0.5 * (static_cast<double>(lo) + hi);
    const double rel[3] = {e[0] - mid * centre[0], e[1] - mid * centre[1], e[2] - mid * centre[2]};
    out.above = static_cast<float>(std::max(dot3(rel, centre), 0.0));

    // Its ground's nearest to the eye: no nearer than at its edge's angle
    // from the eye's direction, at the height there nearest the eye. (The
    // box, widened by the bulge on every side, held the eye 8 km over a
    // 1000 km square, and its texels read as a billion pixels.)
    const double le = std::sqrt(dot3(e, e));
    const double gap = std::max(angleBetween(e, centre) - radius, 0.0);
    const double r = std::clamp(le * std::cos(gap), R + lo, R + hi);
    out.nearest = static_cast<float>(std::sqrt(std::max(le * le + r * r - 2.0 * le * r * std::cos(gap), 0.0)));

    // Hidden: every point of the square, even at its highest, past the
    // body's limb from the eye (the body as low as its lowest ground).
    out.hidden = beyondLimb(body, e, centre, radius, lowest_, hi);
    return out;
}

}  // namespace bro::scene
