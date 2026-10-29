#pragma once

// The body and its place frame (docs/scene-api.js, setBody).
//
// A BODY is the land's shape: a sphere of `radius` metres about its centre
// with a spin axis, or, at an infinite radius, a flat world (the plane
// y = 0, its up +y). One path for both: everything below takes the body's
// map, and the flat body's map is the identity's translations.
//
// The PLACE FRAME is what everything the scene graph holds and the GPU draws
// is in, in float: a frame anchored on the body near the eye, its origin a
// point on the datum, y its radial there, x and z its tangents. The anchors
// lie on a lattice of cells kLattice metres a side: on a flat body the
// squares of the plane (so a frame's origin is a multiple of 2^14 m, and
// every power-of-two lattice the engine keys on maps onto itself); on a
// sphere the cells of the cube's six faces, tan-warped (equal angles, so a
// cell is about kLattice across anywhere on a face). When the eye's nadir
// leaves its anchor's cell by kMargin the frame re-anchors to the cell under
// it: a rigid move (a translation, and on a sphere a tilt of at most a
// lattice step over the radius), which follow() returns for the caller to
// apply to what it holds in the place frame.
//
// Positions on the body are double, in the body's frame (its centre the
// origin on a sphere, the plane's origin on a flat body); nothing at the
// body's scale is ever a float.

#include <bromath/vec.h>

#include <cstdint>
#include <limits>

namespace bro::scene {

struct Body {
    double radius = std::numeric_limits<double>::infinity();   // metres; infinite: flat
    double spin[3] = {0.0, 1.0, 0.0};                           // unit, the body's frame
    bool flat() const { return !(radius < std::numeric_limits<double>::infinity()); }
};

/// The cube's face `f` (0-5): its normal n, and the axes u and v its chart's
/// x and z run along. +Y (2) has a flat body's axes.
struct CubeFace {
    double n[3], u[3], v[3];
};
const CubeFace& cubeFace(int f);

/// The body's CHART: where what is keyed on the ground is keyed. A flat body's is one
/// root, its plane's x and z. A sphere's is the cube's six faces, each a
/// square of face metres: a direction d on face f is at x = R atan(d.u /
/// d.n), z = R atan(d.v / d.n), [-πR/4, πR/4] each way (tan-warped: equal
/// angles, so a face metre is a metre of ground to within a factor of 1.4
/// in area). A face's chart goes on past its edges (the same formula), so a
/// tile's border reads past them.
struct ChartPoint {
    int root = 0;
    double x = 0.0, z = 0.0;
};
/// A sphere's chart point of direction `dir` (unit, the body's frame), on
/// the face it lies on, or on face `root` (extended past its edges; d.n > 0).
ChartPoint chartOf(const Body& body, const double dir[3]);
ChartPoint chartOf(const Body& body, const double dir[3], int root);
/// The direction (unit, the body's frame) of a sphere's chart point.
void dirOf(const Body& body, const ChartPoint& c, double dir[3]);
/// A face's half-width in face metres (πR/4).
double faceHalf(const Body& body);

/// Where a frame is anchored: a lattice cell. A flat body's (0, x, z) is the
/// square [x - 1/2, x + 1/2] x [z - 1/2, z + 1/2] lattice steps about
/// (x, z) * kLattice; a sphere's (face, x, z) is a cell of a cube face, x
/// and z counted from the face's corner.
struct Anchor {
    uint8_t face = 0;
    int32_t x = 0, z = 0;
    bool operator==(const Anchor& o) const { return face == o.face && x == o.x && z == o.z; }
    bool operator!=(const Anchor& o) const { return !(*this == o); }
};

/// A rigid move from one place frame to the next: new = rot * old + t.
struct PlaceMove {
    double rot[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};   // row-major
    double t[3] = {0.0, 0.0, 0.0};
    bool tilts() const;   // rot is not the identity
    void apply(const double in[3], double out[3]) const;
    bromath::Vec3 apply(const bromath::Vec3& p) const;
    /// A direction (a velocity, an up): rot alone.
    bromath::Vec3 turn(const bromath::Vec3& d) const;
};

class Place {
public:
    static constexpr double kLattice = 16384.0;    // a cell's side (m)
    static constexpr double kMargin = 2048.0;      // past its cell's edge before a re-anchor (m)

    /// The body, and the frame anchored on the cell holding `bodyPoint`
    /// (the body's frame; on a sphere its direction from the centre).
    void setBody(const Body& body, const double bodyPoint[3]);
    const Body& body() const { return body_; }
    const Anchor& anchor() const { return anchor_; }
    /// Cells a face side (a sphere's; 0 on a flat body).
    int faceCells() const { return faceCells_; }

    /// The frame: its origin on the body and its axes (x, y, z) as columns,
    /// in the body's frame.
    const double* origin() const { return origin_; }
    const double* basis() const { return basis_; }   // row-major: column j is axis j
    /// The serial of the frame, moved by every re-anchor and setBody.
    uint64_t serial() const { return serial_; }

    void toBody(const double place[3], double out[3]) const;
    void toBody(const bromath::Vec3& place, double out[3]) const;
    void toPlace(const double body[3], double out[3]) const;
    bromath::Vec3 toPlace(const double body[3]) const;

    /// The body's up at a place point, in place axes (unit): +y on a flat
    /// body, the radial through it on a sphere.
    bromath::Vec3 up(const bromath::Vec3& place) const;
    /// Latitude and longitude (radians) of a place point about the spin
    /// axis; a flat body has none (0, 0).
    void latLong(const bromath::Vec3& place, double& lat, double& lon) const;

    /// Follow the eye (a place point): when its nadir is past its anchor's
    /// cell by kMargin, re-anchor on the cell under it and return true with
    /// the move that takes a place point of the old frame to the new.
    bool follow(const bromath::Vec3& eye, PlaceMove& move);
    /// Re-anchor on the cell holding `bodyPoint` whether or not the eye
    /// left its cell; false when that is the anchor already.
    bool anchorAt(const double bodyPoint[3], PlaceMove& move);

    /// The LAND'S FRAME and its MAP:
    /// the ground is drawn in the land's frame (SceneView::world: the body's
    /// frame on a flat body, the place frame on a sphere), and reached by
    /// 2D map coordinates there. A flat body's map is the plane's x and z. A
    /// sphere's is azimuthal-equidistant about the anchor (the place
    /// origin): a point m of the map is the ground |m| metres from the
    /// anchor along the great circle setting off along (m.x, 0, m.z), so it
    /// covers the body to the antipode, and near the anchor it is the place
    /// frame's x and z. Heights are along the ground's up.
    /// The land's-frame point of map point (mx, mz) at height h.
    void mapToLand(double mx, double mz, double h, double out[3]) const;
    /// The map point under land's-frame point p (along its up) and p's
    /// height there.
    void landToMap(const double p[3], double& mx, double& mz, double& h) const;
    /// The chart point of map point (mx, mz).
    ChartPoint chartOfMap(double mx, double mz) const;

    /// The anchor of the cell holding `bodyPoint`.
    Anchor anchorOf(const double bodyPoint[3]) const;
    /// A cell's frame: its origin on the datum and its basis.
    void frameOf(const Anchor& a, double origin[3], double basis[9]) const;

private:
    void set(const Anchor& a);

    Body body_;
    Anchor anchor_;
    int faceCells_ = 0;
    double origin_[3] = {0.0, 0.0, 0.0};
    double basis_[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
    uint64_t serial_ = 1;
};

}  // namespace bro::scene
