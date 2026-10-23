#pragma once

// The cloud deck (docs/scene-world-architecture.md §13, "volumes"; §11,
// "going up"; docs/clouds-api.js): a shell about the body from `bottom` to
// `top` metres over its datum, whose clouds are a world's MODULE, GLSL
// compiled at run time (vulkan/shaders/sky/scene_cloud_layer.glsl says what
// it defines): the cover over the body (baked once into a cube) and the
// extinction at a point of the deck. The engine owns the rest
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
};

}  // namespace bro::scene
