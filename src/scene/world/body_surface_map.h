#pragma once
// A round body's surface map (surface_layers.h, "Roots and the map"): the six cube faces of the body's
// chart (place.h) as the surface layers' roots, each chart square's box in
// the place frame (where a sphere's land is drawn), how high the eye
// stands over it, and whether the body's curve hides it.

#include "scene/world/place.h"
#include "scene/world/surface_layers.h"

namespace bro::scene {

/// Whether every point within `radius` (radians) of body direction `centre`
/// (unit), up to `highest` metres over the datum, is past the body's limb
/// from body point `eye`, the body taken as low as `lowest` (never above
/// the datum).
bool beyondLimb(const Body& body, const double eye[3], const double centre[3], double radius, double lowest,
                double highest);

class BodySurfaceMap final : public SurfaceMap {
public:
    /// Over `place` (it must outlive the map; read as it is at each call),
    /// whose ground is never lower than `lowest` metres (what hides a far
    /// square is the body at that height).
    void set(const Place* place, float lowest) {
        place_ = place;
        lowest_ = lowest;
    }

    int roots() const override { return 6; }
    Patch patch(int root, double x0, double z0, double x1, double z1, float lo, float hi,
                const bromath::Vec3& eye) const override;

private:
    const Place* place_ = nullptr;
    float lowest_ = 0.0f;
};

}  // namespace bro::scene
