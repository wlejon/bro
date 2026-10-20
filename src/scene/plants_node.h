#pragma once

// Plants: trees and shrubs built on the GPU where they stand, from a table
// of SPECIES and a list of PLANTS (where each stands, its species, scale,
// turn, seed and tint). Nothing per leaf is stored: the vertex shader
// (vulkan/shaders/scene_plants.glsl) builds every leaf, limb and trunk from
// its plant's record and its species'.
//
// A plant is a crown of lobes over a trunk with a limb to each; its foliage
// is opaque leaf-shaped elements, as many as its size on screen asks for:
// up close thousands of leaves, far off a few dozen broad daubs, every count
// a prefix of one sequence that covers the whole crown, so the change with
// distance is continuous (vulkan/scene_mesh_drawer_plants.cpp picks the
// counts each frame).
//
// A PlantsNode is a MeshNode so the material setters (receivesShadow,
// castsShadow, visible) apply; it has no mesh, its transform is ignored
// (the plants stand where their records say), and it is left out of
// raycasts and bounds.

#include "scene/mesh_node.h"

#include <cstdint>
#include <vector>

namespace bro::scene {

class PlantsNode : public MeshNode {
public:
    explicit PlantsNode(const std::string& name = "");
    ~PlantsNode() override;

    PlantsNode* asPlants() override { return this; }

    /// A species' record, scene_plants.glsl's PlantSpecies in its order:
    /// height, crown base, crown radius, shape; lobes, lobe size, leaf
    /// length, leaf width; full elements, trunk radius, spread, droop;
    /// light rgb, hue variety; dark rgb, glow; bark rgb, density; glow
    /// colour rgb, needle; 4 unused.
    static constexpr int kSpeciesFloats = 32;
    static constexpr int kMaxSpecies = 64;
    /// A plant: x, y, z (its foot), yaw (radians); scale, species, seed, tint.
    static constexpr int kPlantFloats = 8;

    /// `count` species' records end to end (past kMaxSpecies dropped).
    void setSpecies(const float* data, int count);
    const std::vector<float>& species() const { return species_; }
    int speciesCount() const { return static_cast<int>(species_.size() / kSpeciesFloats); }

    /// `count` plants' records end to end. A plant's species past the
    /// table's is drawn as species 0.
    void setPlants(const float* data, int count);
    const std::vector<float>& plants() const { return plants_; }
    int plantCount() const { return static_cast<int>(plants_.size() / kPlantFloats); }

    struct Params {
        float pixels = 14.0f;      // the size on screen (px) an element is kept to past full detail
        float minElements = 12.0f; // the fewest a plant is drawn with
        float woodRange = 220.0f;  // past this (m) no trunks or limbs are drawn
        float range = 6000.0f;     // past this (m) no plant is drawn
        float shadowTexels = 12.0f; // as `pixels`, in a shadow tile's texels, for its casters
        float shadowRange = 1500.0f; // past this (m) no plant casts (the tiles' frusta cut nearer)
    };
    static constexpr int kParamCount = 6;
    void setParams(const Params& p) { params_ = p; }
    const Params& params() const { return params_; }

    /// A plant's bounding sphere (absolute world: centre, radius), kept with
    /// the records.
    const std::vector<float>& spheres() const { return spheres_; }

    /// Moves when species or plants change (the drawer's cached buffers).
    uint64_t generation() const { return generation_; }

    // What the last frame drew (the drawer sets them).
    struct Stats {
        uint32_t plants = 0;     // plants in view
        uint32_t elements = 0;   // leaf elements drawn
        uint32_t limbs = 0;      // trunks and limbs drawn
        uint32_t shadowElements = 0;   // leaf elements cast, every shadow tile's summed
    };
    Stats& stats() { return stats_; }
    const Stats& stats() const { return stats_; }

private:
    void updateSpheres();

    std::vector<float> species_;
    std::vector<float> plants_;
    std::vector<float> spheres_;
    Params params_;
    uint64_t generation_ = 1;
    Stats stats_;
};

}  // namespace bro::scene
