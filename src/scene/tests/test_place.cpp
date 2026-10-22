// Place (scene/world/place.h), the body and its place frame: a flat body's
// anchors on the 16 km lattice and its exact translations; a sphere's
// cube-face cells (each frame orthonormal, its origin on the datum, its
// cell holding its own origin), the round trip between the frames, a
// re-anchor keeping every point where it is on the body, its tilt under
// a lattice step over the radius, and walks of 100 km, over a face's edge
// and a cube's corner, the eye never far from its anchor.

#include "scene/world/place.h"

#include <cmath>
#include <cstdio>
#include <string>

using namespace bro::scene;

namespace {

int g_checks = 0, g_failures = 0;

void check(bool ok, const std::string& what) {
    ++g_checks;
    if (ok) return;
    if (++g_failures <= 40) std::fprintf(stderr, "FAIL: %s\n", what.c_str());
}

constexpr double kEarth = 6371000.0;
constexpr double kPi = 3.14159265358979323846;

double dist(const double a[3], const double b[3]) {
    return std::sqrt((a[0] - b[0]) * (a[0] - b[0]) + (a[1] - b[1]) * (a[1] - b[1]) + (a[2] - b[2]) * (a[2] - b[2]));
}

void flat() {
    Place p;
    const double zero[3] = {0, 0, 0};
    p.setBody(Body{}, zero);
    check(p.body().flat() && p.anchor() == Anchor{}, "flat: anchored at the origin");
    PlaceMove m;
    check(!p.follow({10000.0f, 3.0f, -10000.0f}, m), "flat: inside the margin, no re-anchor");
    const bromath::Vec3 eye{10300.0f, 2.0f, 5.0f};
    double before[3];
    p.toBody(eye, before);
    const uint64_t serial = p.serial();
    check(p.follow(eye, m), "flat: past the margin, a re-anchor");
    check(p.anchor() == Anchor{0, 1, 0} && p.serial() != serial, "flat: onto the next cell");
    check(!m.tilts() && m.t[0] == -Place::kLattice && m.t[1] == 0.0 && m.t[2] == 0.0, "flat: a lattice step");
    const bromath::Vec3 moved = m.apply(eye);
    double after[3];
    p.toBody(moved, after);
    check(dist(before, after) == 0.0, "flat: the eye where it was, exactly");

    // A 100 km walk, a metre a step: the eye stays within its reach, and its
    // place position keeps a float's millimetres.
    Place w;
    w.setBody(Body{}, zero);
    bromath::Vec3 e{0.0f, 1.7f, 0.0f};
    int moves = 0;
    float worst = 0.0f, error = 0.0f;
    for (int i = 1; i <= 100000; ++i) {
        const double at[3] = {0.8 * i, 1.7, 0.6 * i};
        e = w.toPlace(at);
        PlaceMove mm;
        if (w.follow(e, mm)) e = mm.apply(e), ++moves;
        worst = std::max(worst, std::max(std::fabs(e.x), std::fabs(e.z)));
        double back[3];
        w.toBody(e, back);
        error = std::max(error, static_cast<float>(dist(at, back)));
    }
    check(moves >= 4 && worst <= 0.5f * Place::kLattice + Place::kMargin + 1.0f, "flat walk: re-anchored, near its anchor");
    check(error < 1e-3f, "flat walk: a float place position keeps a millimetre");
}

void sphere() {
    Place p;
    Body b;
    b.radius = kEarth;
    const double north[3] = {0, kEarth, 0};
    p.setBody(b, north);
    check(p.faceCells() == 611, "sphere: Earth's faces of 611 cells");
    check(std::fabs(p.origin()[1] - kEarth) < 1e-3 && p.anchor().face == 2, "sphere: the frame on the datum, +Y face");
    check(std::fabs(p.up({0, 0, 0}).y - 1.0f) < 1e-6f, "sphere: up at the origin is y");
    double lat, lon;
    p.latLong({0, 0, 0}, lat, lon);
    check(std::fabs(lat - 0.5 * kPi) < 0.03, "sphere: the spin axis's pole");

    // Every frame orthonormal, on the datum, its own cell holding its origin.
    for (int f = 0; f < 6; ++f)
        for (int x : {0, 1, 305, 609, 610})
            for (int z : {0, 300, 610}) {
                const Anchor a{static_cast<uint8_t>(f), x, z};
                double o[3], B[9];
                p.frameOf(a, o, B);
                double worst = 0.0;
                for (int i = 0; i < 3; ++i)
                    for (int j = 0; j < 3; ++j) {
                        const double d = B[i] * B[j] + B[3 + i] * B[3 + j] + B[6 + i] * B[6 + j];
                        worst = std::max(worst, std::fabs(d - (i == j ? 1.0 : 0.0)));
                    }
                const double zx = B[1] * B[5] - B[2] * B[4];   // (x cross y).x
                check(worst < 1e-12 && std::fabs(zx - B[6]) < 1e-12, "sphere: frame orthonormal, right-handed");
                check(std::fabs(std::sqrt(o[0] * o[0] + o[1] * o[1] + o[2] * o[2]) - kEarth) < 1e-6, "sphere: origin on the datum");
                check(p.anchorOf(o) == a, "sphere: a cell holds its origin");
            }

    // Round trip and a re-anchor keep body positions.
    const bromath::Vec3 eye{10500.0f, 120.0f, -3000.0f};
    double before[3], back[3];
    p.toBody(eye, before);
    p.toPlace(before, back);
    check(std::fabs(back[0] - eye.x) < 1e-6 && std::fabs(back[1] - eye.y) < 1e-6 && std::fabs(back[2] - eye.z) < 1e-6,
          "sphere: place to body and back");
    PlaceMove m;
    check(p.follow(eye, m) && m.tilts(), "sphere: a re-anchor tilts");
    double moved[3], after[3];
    m.apply(back, moved);
    p.toBody(moved, after);
    check(dist(before, after) < 1e-6, "sphere: the eye where it was");
    const double tilt = std::acos(std::min(1.0, m.rot[4]));
    check(tilt < 0.2 * kPi / 180.0, "sphere: tilt under 0.2 degrees");
    const bromath::Vec3 e2 = m.apply(eye);
    check(std::fabs(e2.x) < 0.5 * Place::kLattice + 1.0 && std::fabs(e2.z) < 0.5 * Place::kLattice + 1.0, "sphere: the eye near its new anchor");
}

// A walk along a great circle through a face's edge and near a corner:
// each step a metre on the ground, the eye re-anchoring as it goes.
void walk(const double startDir[3], const double headingDir[3], double km, const char* what) {
    Place p;
    Body b;
    b.radius = kEarth;
    double s[3] = {startDir[0] * kEarth, startDir[1] * kEarth, startDir[2] * kEarth};
    p.setBody(b, s);
    bromath::Vec3 eye = p.toPlace(s);
    eye.y += 1.7f;
    // The heading, in place axes, along the ground.
    const double far[3] = {s[0] + headingDir[0], s[1] + headingDir[1], s[2] + headingDir[2]};
    bromath::Vec3 h = p.toPlace(far) - p.toPlace(s);
    h.y = 0.0f;
    h = bromath::vnorm(h);
    int moves = 0;
    float worst = 0.0f, drift = 0.0f;
    const int steps = static_cast<int>(km * 1000.0);
    for (int i = 0; i < steps; ++i) {
        // Step along the ground and back onto the sphere's 1.7 m shell.
        eye = eye + h;
        double bp[3];
        p.toBody(eye, bp);
        const double r = std::sqrt(bp[0] * bp[0] + bp[1] * bp[1] + bp[2] * bp[2]);
        for (double& c : bp) c *= (kEarth + 1.7) / r;
        eye = p.toPlace(bp);
        const bromath::Vec3 up = p.up(eye);
        h = bromath::vnorm(h - up * bromath::vdot(h, up));
        PlaceMove m;
        if (p.follow(eye, m)) {
            eye = m.apply(eye);
            h = m.turn(h);
            ++moves;
        }
        worst = std::max(worst, std::max(std::fabs(eye.x), std::fabs(eye.z)));
        drift = std::max(drift, std::fabs(eye.y - 1.7f));
    }
    check(moves >= static_cast<int>(km / 16.384 / 1.5) && worst <= 0.5f * Place::kLattice + Place::kMargin + 2.0f,
          std::string(what) + ": re-anchored, near its anchor");
    check(drift < 30.0f, std::string(what) + ": the shell's height in the place frame stays small");
}

}  // namespace

int main() {
    flat();
    sphere();
    const double xEdge[3] = {0.7071067811865476, 0.7071067811865476, 0.0};   // on +X/+Y's edge
    const double toward[3] = {1.0, -1.0, 0.0};
    walk(xEdge, toward, 100.0, "walk over a face's edge");
    const double corner[3] = {0.5773502691896258, 0.5773502691896258, 0.5773502691896258};
    const double across[3] = {1.0, 0.0, -1.0};
    walk(corner, across, 60.0, "walk past a cube's corner");
    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
