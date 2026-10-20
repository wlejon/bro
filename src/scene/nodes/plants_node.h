#pragma once

// Plants: trees and shrubs built on the GPU where they stand, from a table
// of SPECIES and a list of PLANTS (where each stands, its species, scale,
// turn, seed and tint). Nothing per leaf is stored: the vertex shader
// (vulkan/shaders/foliage/scene_plants.glsl) builds every leaf, limb and trunk from
// its plant's record and its species'.
//
// A plant is a crown of lobes over a trunk with a limb to each; its foliage
// is opaque leaf-shaped elements, as many as its size on screen asks for:
// up close thousands of leaves, far off a few dozen broad daubs, every count
// a prefix of one sequence that covers the whole crown, so the change with
// distance is continuous (vulkan/foliage/scene_plants_gpu_plan.cpp picks the
// counts each frame, on the GPU).
//
// The plants are a world-cell provider (world/world_cells.h): setPlants sorts
// them into cell order once, and the records the GPU reads (gpuData) are in
// that order, uploaded when they or the species change and never per frame.
// A frame's work then follows the cells in view, not the plant count.
//
// A PlantsNode is a MeshNode so the material setters (receivesShadow,
// castsShadow, visible) apply; it has no mesh, its transform is ignored
// (the plants stand where their records say), and it is left out of
// raycasts and bounds.

#include "scene/nodes/mesh_node.h"
#include "scene/world/world_cells.h"

#include <cstdint>
#include <memory>
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
    /// gpuData's header, before the species: plants, species, 0, the first
    /// species record (in vec4s, from the records' start); the first plant
    /// record, 0, 0, 0; then 8 zeros (scene_plants.glsl's header[4]).
    static constexpr int kHeaderFloats = 16;

    /// `count` species' records end to end (past kMaxSpecies dropped).
    void setSpecies(const float* data, int count);
    int speciesCount() const { return speciesCount_; }
    /// Species `s`'s record.
    const float* species(int s) const { return data_.data() + kHeaderFloats + static_cast<size_t>(s) * kSpeciesFloats; }

    /// `count` plants' records end to end, kept in cell order (not the
    /// order given). A plant's species past the table's is drawn as species 0.
    void setPlants(const float* data, int count);
    int plantCount() const { return plantCount_; }
    /// Plant `i`'s record, in cell order.
    const float* plant(int i) const { return data_.data() + plantsAt_ + static_cast<size_t>(i) * kPlantFloats; }

    /// The header, the species and the plants as the GPU reads them (set 3
    /// binding 1 of the plants' draws, scene_plants.glsl).
    const std::vector<float>& gpuData() const { return data_; }

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

    /// The plants' cells (world_cells.h): each leaf's run of the cell order,
    /// bounded by its plants' bounding spheres' boxes (a little wider: the
    /// GPU works the spheres out again in its own arithmetic, the plan's
    /// plantsSphere).
    const CellGrid& cellGrid() const { return grid_; }
    const std::vector<CellContent>& cells() const { return cells_; }

    /// Moves when species or plants change (the drawer's cached buffers).
    uint64_t generation() const { return generation_; }

    // What a frame drew.
    struct Stats {
        uint32_t plants = 0;     // plants in view
        uint32_t elements = 0;   // leaf elements drawn
        uint32_t limbs = 0;      // trunks and limbs drawn
        uint32_t shadowElements = 0;   // leaf elements cast, every shadow tile's summed
    };
    /// Where the stats come from: the drawer plans the node on the GPU
    /// (scene_plants_gpu_plan.cpp), a frame's read back once the GPU has
    /// drawn it, a frame or two late, and stats() takes the newest finished.
    class StatsSource {
    public:
        virtual ~StatsSource() = default;
        /// The newest finished frame's stats into `out` (false: none newer).
        virtual bool latest(Stats& out) = 0;
    };
    void setStatsSource(std::weak_ptr<StatsSource> source) { statsSource_ = std::move(source); }
    void setStats(const Stats& s) {
        stats_ = s;
        statsSource_.reset();
    }
    const Stats& stats() const {
        if (auto source = statsSource_.lock()) source->latest(stats_);
        return stats_;
    }

private:
    void rebuild();

    CellGrid grid_;
    std::vector<float> data_;       // header, species, plants (cell order)
    size_t plantsAt_ = kHeaderFloats;   // the first plant's float in data_
    int speciesCount_ = 0, plantCount_ = 0;
    std::vector<CellContent> cells_;
    Params params_;
    uint64_t generation_ = 0;
    mutable Stats stats_;
    std::weak_ptr<StatsSource> statsSource_;
};

}  // namespace bro::scene
