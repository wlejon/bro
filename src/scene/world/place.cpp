#include "scene/world/place.h"

#include <algorithm>
#include <cmath>

namespace bro::scene {

namespace {

constexpr double kPi = 3.14159265358979323846;

// The cube's faces: normal n, then the axes u and v a cell's x and z run
// along. +Y's u and v are the flat body's x and z, so a frame at that
// face's centre has a flat world's axes.
using Face = CubeFace;
constexpr Face kFaces[6] = {
    {{1, 0, 0}, {0, 0, -1}, {0, 1, 0}},
    {{-1, 0, 0}, {0, 0, 1}, {0, 1, 0}},
    {{0, 1, 0}, {1, 0, 0}, {0, 0, 1}},
    {{0, -1, 0}, {1, 0, 0}, {0, 0, -1}},
    {{0, 0, 1}, {1, 0, 0}, {0, 1, 0}},
    {{0, 0, -1}, {-1, 0, 0}, {0, 1, 0}},
};

double dot(const double a[3], const double b[3]) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }

void cross(const double a[3], const double b[3], double out[3]) {
    out[0] = a[1] * b[2] - a[2] * b[1];
    out[1] = a[2] * b[0] - a[0] * b[2];
    out[2] = a[0] * b[1] - a[1] * b[0];
}

bool normalize(double v[3]) {
    const double l = std::sqrt(dot(v, v));
    if (!(l > 0.0)) return false;
    v[0] /= l;
    v[1] /= l;
    v[2] /= l;
    return true;
}

}  // namespace

const CubeFace& cubeFace(int f) { return kFaces[std::clamp(f, 0, 5)]; }

double faceHalf(const Body& body) { return body.flat() ? 0.0 : 0.25 * kPi * body.radius; }

ChartPoint chartOf(const Body& body, const double dir[3], int root) {
    const Face& F = kFaces[std::clamp(root, 0, 5)];
    const double n = dot(dir, F.n);
    return {root, body.radius * std::atan2(dot(dir, F.u), n), body.radius * std::atan2(dot(dir, F.v), n)};
}

ChartPoint chartOf(const Body& body, const double dir[3]) {
    int face = 0;
    double best = -2.0;
    for (int f = 0; f < 6; ++f) {
        const double c = dot(dir, kFaces[f].n);
        if (c > best) best = c, face = f;
    }
    return chartOf(body, dir, face);
}

void dirOf(const Body& body, const ChartPoint& c, double dir[3]) {
    const Face& F = kFaces[std::clamp(c.root, 0, 5)];
    const double tx = std::tan(c.x / body.radius), tz = std::tan(c.z / body.radius);
    for (int i = 0; i < 3; ++i) dir[i] = F.n[i] + tx * F.u[i] + tz * F.v[i];
    normalize(dir);
}

bool PlaceMove::tilts() const {
    for (int i = 0; i < 9; ++i)
        if (rot[i] != ((i % 4) == 0 ? 1.0 : 0.0)) return true;
    return false;
}

void PlaceMove::apply(const double in[3], double out[3]) const {
    double r[3];
    for (int i = 0; i < 3; ++i) r[i] = rot[i * 3] * in[0] + rot[i * 3 + 1] * in[1] + rot[i * 3 + 2] * in[2] + t[i];
    out[0] = r[0];
    out[1] = r[1];
    out[2] = r[2];
}

bromath::Vec3 PlaceMove::apply(const bromath::Vec3& p) const {
    const double in[3] = {p.x, p.y, p.z};
    double out[3];
    apply(in, out);
    return {static_cast<float>(out[0]), static_cast<float>(out[1]), static_cast<float>(out[2])};
}

bromath::Vec3 PlaceMove::turn(const bromath::Vec3& d) const {
    double out[3];
    for (int i = 0; i < 3; ++i) out[i] = rot[i * 3] * d.x + rot[i * 3 + 1] * d.y + rot[i * 3 + 2] * d.z;
    return {static_cast<float>(out[0]), static_cast<float>(out[1]), static_cast<float>(out[2])};
}

void Place::setBody(const Body& body, const double bodyPoint[3]) {
    body_ = body;
    normalize(body_.spin);
    faceCells_ = body_.flat() ? 0 : std::max(1, static_cast<int>(std::lround(0.5 * kPi * body_.radius / kLattice)));
    set(anchorOf(bodyPoint));
}

void Place::set(const Anchor& a) {
    anchor_ = a;
    frameOf(a, origin_, basis_);
    ++serial_;
}

Anchor Place::anchorOf(const double p[3]) const {
    if (body_.flat())
        return {0, static_cast<int32_t>(std::lround(p[0] / kLattice)), static_cast<int32_t>(std::lround(p[2] / kLattice))};
    double d[3] = {p[0], p[1], p[2]};
    if (!normalize(d)) d[1] = 1.0;
    int face = 0;
    double best = -1.0;
    for (int f = 0; f < 6; ++f) {
        const double c = dot(d, kFaces[f].n);
        if (c > best) best = c, face = f;
    }
    const Face& F = kFaces[face];
    const double step = 0.5 * kPi / faceCells_;
    const auto cell = [&](const double axis[3]) {
        const double a = std::atan(dot(d, axis) / best) + 0.25 * kPi;
        return std::clamp(static_cast<int32_t>(std::floor(a / step)), 0, faceCells_ - 1);
    };
    return {static_cast<uint8_t>(face), cell(F.u), cell(F.v)};
}

void Place::frameOf(const Anchor& a, double origin[3], double basis[9]) const {
    if (body_.flat()) {
        origin[0] = a.x * kLattice;
        origin[1] = 0.0;
        origin[2] = a.z * kLattice;
        for (int i = 0; i < 9; ++i) basis[i] = (i % 4) == 0 ? 1.0 : 0.0;
        return;
    }
    const Face& F = kFaces[a.face];
    const double step = 0.5 * kPi / faceCells_;
    const double tx = std::tan(-0.25 * kPi + (a.x + 0.5) * step);
    const double tz = std::tan(-0.25 * kPi + (a.z + 0.5) * step);
    double y[3], x[3], z[3];
    for (int i = 0; i < 3; ++i) y[i] = F.n[i] + tx * F.u[i] + tz * F.v[i];
    normalize(y);
    // x: the face's u along the ground there; z completes a right-handed
    // frame (x cross y), as a flat body's (1,0,0) x (0,1,0) = (0,0,1).
    const double uy = dot(F.u, y);
    for (int i = 0; i < 3; ++i) x[i] = F.u[i] - uy * y[i];
    normalize(x);
    cross(x, y, z);
    for (int i = 0; i < 3; ++i) {
        origin[i] = body_.radius * y[i];
        basis[i * 3] = x[i];
        basis[i * 3 + 1] = y[i];
        basis[i * 3 + 2] = z[i];
    }
}

void Place::toBody(const double p[3], double out[3]) const {
    for (int i = 0; i < 3; ++i)
        out[i] = origin_[i] + basis_[i * 3] * p[0] + basis_[i * 3 + 1] * p[1] + basis_[i * 3 + 2] * p[2];
}

void Place::toBody(const bromath::Vec3& p, double out[3]) const {
    const double in[3] = {p.x, p.y, p.z};
    toBody(in, out);
}

void Place::toPlace(const double b[3], double out[3]) const {
    const double d[3] = {b[0] - origin_[0], b[1] - origin_[1], b[2] - origin_[2]};
    for (int j = 0; j < 3; ++j) out[j] = basis_[j] * d[0] + basis_[3 + j] * d[1] + basis_[6 + j] * d[2];
}

bromath::Vec3 Place::toPlace(const double b[3]) const {
    double out[3];
    toPlace(b, out);
    return {static_cast<float>(out[0]), static_cast<float>(out[1]), static_cast<float>(out[2])};
}

bromath::Vec3 Place::up(const bromath::Vec3& place) const {
    if (body_.flat()) return {0.0f, 1.0f, 0.0f};
    double b[3];
    toBody(place, b);
    if (!normalize(b)) return {0.0f, 1.0f, 0.0f};
    double out[3];
    for (int j = 0; j < 3; ++j) out[j] = basis_[j] * b[0] + basis_[3 + j] * b[1] + basis_[6 + j] * b[2];
    return {static_cast<float>(out[0]), static_cast<float>(out[1]), static_cast<float>(out[2])};
}

void Place::latLong(const bromath::Vec3& place, double& lat, double& lon) const {
    lat = lon = 0.0;
    if (body_.flat()) return;
    double r[3];
    toBody(place, r);
    if (!normalize(r)) return;
    const double* s = body_.spin;
    // Longitude from the meridian through the axis nearest the body's +x
    // (+y when the spin is near x).
    double e1[3] = {std::fabs(s[0]) < 0.9 ? 1.0 : 0.0, std::fabs(s[0]) < 0.9 ? 0.0 : 1.0, 0.0};
    const double k = dot(e1, s);
    for (int i = 0; i < 3; ++i) e1[i] -= k * s[i];
    normalize(e1);
    double e2[3];
    cross(s, e1, e2);
    lat = std::asin(std::clamp(dot(r, s), -1.0, 1.0));
    lon = std::atan2(dot(r, e2), dot(r, e1));
}

void Place::mapToLand(double mx, double mz, double h, double out[3]) const {
    if (body_.flat()) {
        out[0] = mx;
        out[1] = h;
        out[2] = mz;
        return;
    }
    const double R = body_.radius;
    const double r = std::sqrt(mx * mx + mz * mz);
    const double theta = r / R;
    const double s = r > 0.0 ? std::sin(theta) / r : 1.0 / R;   // sin θ over |m|
    const double half = std::sin(0.5 * theta);
    out[0] = (R + h) * s * mx;
    out[1] = h * std::cos(theta) - 2.0 * R * half * half;
    out[2] = (R + h) * s * mz;
}

void Place::landToMap(const double p[3], double& mx, double& mz, double& h) const {
    if (body_.flat()) {
        mx = p[0];
        mz = p[2];
        h = p[1];
        return;
    }
    const double R = body_.radius;
    const double y = R + p[1];
    const double rho = std::sqrt(p[0] * p[0] + p[2] * p[2]);
    const double theta = std::atan2(rho, y);
    const double k = rho > 0.0 ? R * theta / rho : 1.0;
    mx = k * p[0];
    mz = k * p[2];
    h = std::sqrt(rho * rho + y * y) - R;
}

ChartPoint Place::chartOfMap(double mx, double mz) const {
    if (body_.flat()) return {0, mx, mz};
    const double r = std::sqrt(mx * mx + mz * mz);
    const double theta = r / body_.radius;
    const double s = r > 0.0 ? std::sin(theta) / r : 0.0;
    const double local[3] = {s * mx, std::cos(theta), s * mz};
    double d[3];
    for (int i = 0; i < 3; ++i) d[i] = basis_[i * 3] * local[0] + basis_[i * 3 + 1] * local[1] + basis_[i * 3 + 2] * local[2];
    return chartOf(body_, d);
}

bool Place::follow(const bromath::Vec3& eye, PlaceMove& move) {
    const double reach = 0.5 * kLattice + kMargin;
    if (std::fabs(eye.x) <= reach && std::fabs(eye.z) <= reach) return false;
    double b[3];
    toBody(eye, b);
    return anchorAt(b, move);
}

bool Place::anchorAt(const double bodyPoint[3], PlaceMove& move) {
    const Anchor a = anchorOf(bodyPoint);
    if (a == anchor_) return false;
    double o[3], B[9];
    frameOf(a, o, B);
    // new = Bn^T (Bo old + Oo - On): rot = Bn^T Bo, t = Bn^T (Oo - On).
    const double d[3] = {origin_[0] - o[0], origin_[1] - o[1], origin_[2] - o[2]};
    for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 3; ++j)
            move.rot[i * 3 + j] = B[i] * basis_[j] + B[3 + i] * basis_[3 + j] + B[6 + i] * basis_[6 + j];
        move.t[i] = B[i] * d[0] + B[3 + i] * d[1] + B[6 + i] * d[2];
    }
    if (body_.flat()) {
        // Exact: a lattice step is a whole power of two, and no tilt.
        for (int i = 0; i < 9; ++i) move.rot[i] = (i % 4) == 0 ? 1.0 : 0.0;
    }
    set(a);
    return true;
}

}  // namespace bro::scene
