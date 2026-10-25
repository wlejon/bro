#pragma once

// The cloud deck (docs/scene-world-architecture.md §13, "volumes"; §11,
// "going up"; docs/clouds-api.js): a shell about the body from `bottom` to
// `top` metres over its datum, whose clouds are a world's MODULE, GLSL
// compiled at run time (vulkan/shaders/sky/scene_cloud_layer.glsl says what
// it defines): the cover over the body (baked once into a cube) and the
// base at a point of the deck (baked into the cache about the eye), which
// the engine cuts into its forms. The engine owns the rest
// (vulkan/sky/scene_clouds.h): the march through the deck from any
// altitude against the scene's depth, the light through the air and the
// deck, the air before it, and its shadow on the land.
//
// It needs the atmosphere (the deck's light and the air before it are the
// atmosphere's): with the atmosphere off, nothing is drawn. The deck lies on
// the atmosphere's planet (its radius the body's on a round body).

#include <cstdint>
#include <string>
#include <vector>

namespace bro::scene {

struct CloudSettings {
    bool enabled = false;
    /// The module (cloudCover and cloudDensity) and its hash (0: none).
    std::string source;
    uint64_t sourceHash = 0;
    float bottom = 1500.0f;      // the deck's bottom, m over the datum
    float top = 8000.0f;         // its top
    float albedo = 0.98f;        // what a droplet scatters of what it takes out of the beam
    bool shadows = true;         // its shadow on the land
    /// The engine's forms (docs/clouds-api.js): a cloud's heart's
    /// extinction (1/m), the billows' tile (m), the detail's tile (m) and
    /// the metres over which the world's base rises from none to all at a
    /// cloud's edge (what sizes the skin the forms move); the wind they
    /// drift with (m/s, the body's frame).
    float extinction = 0.06f;
    float billowScale = 4000.0f;
    float detailScale = 1200.0f;
    float edge = 600.0f;
    float wind[3] = {0.0f, 0.0f, 0.0f};
    /// cloudData(i): at most kMaxData vec4s, as floats.
    std::vector<float> data;
    static constexpr size_t kMaxData = 64;
    /// The land whose processes' grid the cover reads (cloudLandGrid; 0 none).
    uint32_t land = 0;
    /// Moved by every setClouds: the cover is baked again.
    uint64_t generation = 0;
};

/// What the GPU side made of the deck (scene.cloudStats()).
struct CloudStats {
    bool compiled = false;   // its module compiled
    bool failed = false;     // its module did not compile (logged)
    bool covered = false;    // its cover is baked for this generation
    uint64_t generation = 0; // the generation the cover is baked for
    // The cache's bakes still to come for this view: levels to bake again
    // and slices of rows not yet made since the last whole change (0 when
    // it has converged; scene settle state).
    int cachePending = 0;
    // The volume about the eye (vulkan/sky/scene_cloud_volume.h): its bakes
    // still to come for this view (levels to bake whole again and slices of
    // rows not yet made since the last whole change; settle state), its
    // memory, the cells it baked the newest frame and the slack its
    // distances are read less (m).
    int volumePending = 0;
    double volumeBytes = 0.0;
    double volumeCells = 0.0;
    float volumeSlack = 0.0f;
    // Its newest check (BRO_CLOUD_VOLUME_CHECK=1; serial 0: none yet):
    // rays walked on the volume; at each point its extinction against the
    // forms' at the level's cell (the mean absolute error, the forms' mean
    // and most), and inside each jump the forms the march would draw (the
    // points sampled, the violations: cloud inside a jump).
    struct VolumeCheck {
        uint32_t serial = 0;
        int rays = 0;
        double compared = 0.0, meanAbsError = 0.0, meanSigma = 0.0, mostSigma = 0.0;
        double inside = 0.0, violations = 0.0, jumps = 0.0, meanJump = 0.0;
    } volumeCheck;
};

}  // namespace bro::scene
