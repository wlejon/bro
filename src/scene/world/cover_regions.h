#pragma once

// The cover's regions: what a grass node's blades, herbs and far tier are keyed on.
//
// A flat body's cover is keyed on the plane: one region, the land's own
// frame. A sphere has no flat chart, so its cover is laid out on the cells
// of the place frame's lattice (place.h), each its own flat frame: the
// frame the place would have anchored on that cell, reached by its
// azimuthal map (x and z about its middle) and the height over the datum.
// A draw is a region's (its cells, its eye, everything its modules work out
// in that flat frame), and the shaders stand what it draws on the body
// (scene_grass.glsl's coverToEye, coverMap). What stands at a point is the
// region's whose chart cell holds it (coverOwns), so a place's cover is
// the same wherever the place frame is anchored.

#include "scene/world/place.h"

#include <bromath/aabb.h>
#include <bromath/vec.h>

#include <vector>

namespace bro::scene {

struct CoverRegion {
    Anchor cell;
    bool flat = true;   // the land's own frame (the rest below is the identity's)
    /// The draw's words (scene_grass.glsl's GrassUBO frame, own, own2): rows
    /// of its rotation into the land's frame and its middle there, the eye
    /// in it, round; its chart bounds about the anchor's; its face, the
    /// hand-over band, the half side of the square about its middle wholly
    /// its own.
    float frame[16] = {};
    float own[4] = {};
    float own2[4] = {};
    /// The eye in its flat frame: map x, height over the datum, map z.
    bromath::Vec3 eye{0.0f, 0.0f, 0.0f};
    /// Its flat frame's square about its middle holding all of its cell and
    /// its band (half a side; infinite on a flat body).
    float reach = 0.0f;
    /// Its rotation into the land's frame (row-major), and where its middle
    /// is there, in double for the CPU's boxes.
    double rot[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
    double origin[3] = {0.0, 0.0, 0.0};

    /// Its flat point (map x, z, height h) in the land's frame.
    void toLand(const Place& place, double x, double z, double h, double out[3]) const;
    /// The land's map box under its flat box [x0, x1] x [z0, z1].
    void landMapBox(const Place& place, double x0, double z0, double x1, double z1, double out[4]) const;
    /// The land's-frame box holding its flat box between heights lo and hi.
    bromath::AABB3 landBox(const Place& place, float x0, float z0, float x1, float z1, float lo, float hi) const;
    /// A direction of the land's frame in its flat frame.
    bromath::Vec3 fromLand(const bromath::Vec3& d) const;
};

/// The regions whose ground lies within `reach` metres of the eye (a point
/// of the land's frame: SceneView::world's), each handing over to its
/// neighbours through a band of `band` metres; a flat body's one.
void coverRegions(const Place& place, const bromath::Vec3& eye, double reach, double band,
                  std::vector<CoverRegion>& out);

}  // namespace bro::scene
