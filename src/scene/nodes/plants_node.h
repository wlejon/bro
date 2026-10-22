#pragma once

// Plants: trees and shrubs built on the GPU where they stand, from a table
// of SPECIES and the plants of each world cell (where each stands, its
// species, scale, turn, seed and tint). Nothing per element is stored: the
// GPU builds every element, limb and trunk from its plant's record and its
// species'.
//
// What a plant is is the world's: a MODULE, GLSL compiled into the plants'
// shaders at run time (setModule, vulkan/shaders/foliage/scene_plant_layer.glsl;
// docs/scene-world-architecture.md §13), which reads its share of each
// species' record. A node with no module draws nothing. The engine's share
// is the plant's size, its detail and its crown as the light sees it.
//
// A plant's foliage is opaque elements, as many as its size on screen asks
// for: up close thousands, far off a few dozen broad daubs, every count a
// prefix of the module's one sequence, so the change with distance is
// continuous (vulkan/foliage/scene_plants_gpu_plan.cpp picks the counts each
// frame, on the GPU).
//
// Where the plants are (docs/scene-world-architecture.md §10): placed by
// RULES (world/plant_placement.h), the leaves about the eye made from the
// world's fields as it moves (follow(), each frame, the camera's eye) and
// dropped behind it, so a world of any size costs what is in reach; and any
// plants set by hand (setPlants), kept always. Both live in one store by
// world cell (world/cell_store.h): a leaf's plants a run of its records,
// the GPU copy taking only the runs that changed. The plants are a
// world-cell provider (world/world_cells.h): a frame's work follows the
// cells in view, not the plant count.
//
// A PlantsNode is a MeshNode so the material setters (receivesShadow,
// castsShadow, visible) apply; it has no mesh, its transform is ignored
// (the plants stand where their records say), and it is left out of
// raycasts and bounds.

#include "scene/nodes/mesh_node.h"
#include "scene/world/cell_store.h"
#include "scene/world/place.h"
#include "scene/world/world_cells.h"

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <vector>

namespace bro::scene {

class PlantPlacement;
struct PlacementRules;

class PlantsNode : public MeshNode {
public:
    explicit PlantsNode(const std::string& name = "");
    ~PlantsNode() override;

    PlantsNode* asPlants() override { return this; }

    /// A species' record: the engine's share (scene_plant_records.glsl's
    /// PlantSpecies) height, crown base, crown radius, full elements;
    /// element length, limbs, crown shape, density; mean foliage rgb, 0;
    /// then the module's kSpeciesDataFloats (its plantData).
    static constexpr int kSpeciesFloats = 32;
    static constexpr int kSpeciesEngineFloats = 12;
    static constexpr int kSpeciesDataFloats = kSpeciesFloats - kSpeciesEngineFloats;
    static constexpr int kMaxSpecies = 64;
    /// A plant: x, y, z (its foot), yaw (radians); scale, species, seed, tint.
    static constexpr int kPlantFloats = 8;
    /// gpuData's header, before the species: the pool's room (in plants),
    /// species, 0, the first species record (in vec4s, from the records'
    /// start); the first plant record, 0, 0, 0; then 8 zeros
    /// (scene_plants.glsl's header[4]). The species table has room for
    /// kMaxSpecies, so the pool always starts at the same place.
    static constexpr int kHeaderFloats = 16;
    static constexpr int kPrefixFloats = kHeaderFloats + kMaxSpecies * kSpeciesFloats;

    /// The world's plant module ("" none: the node draws nothing).
    void setModule(std::string source);
    const std::string& moduleSource() const { return moduleSource_; }
    uint64_t moduleSourceHash() const { return moduleSourceHash_; }

    /// `count` species' records end to end (past kMaxSpecies dropped).
    void setSpecies(const float* data, int count);
    int speciesCount() const { return speciesCount_; }
    /// Species `s`'s record.
    const float* species(int s) const {
        return store_.data().data() + kHeaderFloats + static_cast<size_t>(s) * kSpeciesFloats;
    }

    /// `count` plants' records end to end, placed by hand: each kept in its
    /// cell, whatever the eye does.
    void setPlants(const float* data, int count);
    /// The rules the plants about the eye are placed by (null: none).
    void setRules(std::shared_ptr<const PlacementRules> rules);
    const PlacementRules* rules() const;
    /// The eye this frame (the camera's, in the frame the plants stand in:
    /// SceneView::world): the leaves placed by the rules come in about it
    /// and go behind it. Never waits for a placement; a cell not placed yet
    /// is not drawn yet.
    void follow(const bromath::Vec3& eye);
    /// The body it stands on, its graph's (SceneGraph keeps it set; null or
    /// a flat body: the plane). On a round body the rules place on its
    /// chart (world/plant_placement.h) and the plants stand in the place
    /// frame; a new body places them again.
    void setPlace(const Place* place);
    /// The place frame moved (a re-anchor, SceneGraph::applyPlaceMove): on
    /// a round body every plant takes the move, so it stands where it stood
    /// on the body.
    void placeMoved(const PlaceMove& move);
    /// Placement's leaves wanted and not yet placed.
    size_t placementPending() const;

    /// The plants in the store now.
    int plantCount() const { return static_cast<int>(store_.live()); }
    /// The tallest a plant stands (its species' height by its scale).
    float tallest() const { return tallest_; }

    /// The header, the species and the plants' pool as the GPU reads them
    /// (set 3 binding 1 of the plants' draws, scene_plants.glsl), and how
    /// they change (CellStore: the runs changed since a serial).
    const CellStore& store() const { return store_; }
    const std::vector<float>& gpuData() const { return store_.data(); }

    struct Params {
        float pixels = 14.0f;      // the size on screen (px) an element is kept to past full detail
        float minElements = 12.0f; // the fewest a plant is drawn with
        float woodRange = 220.0f;  // past this (m) no trunks or limbs are drawn
        float range = 6000.0f;     // past this (m) no plant is drawn
        float shadowTexels = 12.0f; // as `pixels`, in a shadow page's texels, for its casters
        float shadowRange = 1500.0f; // past this (m), or `range`, no plant casts (the shadow views cut nearer)
    };
    static constexpr int kParamCount = 6;
    void setParams(const Params& p);
    const Params& params() const { return params_; }

    /// The plants' cells (world_cells.h): each leaf's run of the pool,
    /// bounded by its plants' bounding spheres' boxes (a little wider: the
    /// GPU works the spheres out again in its own arithmetic, the plan's
    /// plantsSphere), in quadtree order.
    const CellGrid& cellGrid() const { return grid_; }
    const std::vector<CellContent>& cells() const;

    /// Moves when the cells change (a leaf placed, dropped or set, the
    /// species).
    uint64_t generation() const { return generation_; }
    /// The boxes of what changed after `generation` (each changed leaf's
    /// old and new bounds); false when the log no longer reaches back so
    /// far, or a change touched everything (the species): take all of it
    /// as changed.
    bool changesSince(uint64_t generation, std::vector<bromath::AABB3>& out) const;

    /// About how many leaf elements leaf `key`'s plants cast into shadow
    /// views of finestTexel * 2^level metres, levels [from, to), into
    /// `out[level]` (the plan's count, scene_plants_gpu_plan.cpp, a crown a
    /// disc counted one): what drawing them there costs. Kept with the leaf
    /// until it changes.
    void castElements(const CellKey& key, float finestTexel, int from, int to, float* out) const;

    // What a frame drew.
    struct Stats {
        uint32_t plants = 0;     // plants in view
        uint32_t elements = 0;   // leaf elements drawn
        uint32_t limbs = 0;      // trunks and limbs drawn
        uint32_t shadowElements = 0;   // leaf elements cast, every shadow view's summed
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
    /// The frames whose plan ran out of room (its records or leaves, sized
    /// from the frames before: a jump into denser plants), by their shadow
    /// plan's serial, as their read-back comes in. What such a frame drew
    /// was short of plants, and a shadow page keeps what it was drawn with:
    /// the renderer takes them and redraws the pages those frames drew.
    void notePlanShort(uint64_t shadowSerial) const { planShort_.push_back(shadowSerial); }
    void takePlanShort(std::vector<uint64_t>& out) const {
        out.clear();
        out.swap(planShort_);
    }

private:
    // A leaf's plants: those set by hand and how many placed follow them in
    // its run, the run's bounds, and how many of each species (at their
    // mean scale) it holds.
    struct Kind {
        int species = 0;
        uint32_t count = 0;
        float scale = 0.0f;
    };
    struct Leaf {
        std::vector<float> fixed;
        uint32_t placed = 0;
        bromath::AABB3 bounds = bromath::aempty3();
        std::vector<Kind> kinds;
        // castElements' last answer, and what it was asked.
        mutable std::vector<float> cast;
        mutable float castFinest = 0.0f;
        mutable int castFrom = 0;
    };
    struct Before {
        const CellGrid* grid;
        bool operator()(const CellKey& a, const CellKey& b) const { return grid->before(a, b); }
    };
    using Leaves = std::map<CellKey, Leaf, Before>;
    /// Leaf `it`'s run made again from its fixed plants and `placed` (null:
    /// keep the placed it has; an empty leaf goes).
    void putLeaf(Leaves::iterator it, const std::vector<float>* placed);
    /// Leaf's bounds and kinds from its run.
    void measure(Leaf& leaf, uint32_t first, uint32_t count);
    void writeHeader();
    void changedAll();
    static constexpr uint8_t kHandRoot = 7;   // the plants set by hand on a round body

    std::string moduleSource_;
    uint64_t moduleSourceHash_ = 0;
    CellGrid grid_;
    CellStore store_{kPrefixFloats, kPlantFloats};
    Leaves leaves_{Before{&grid_}};
    int speciesCount_ = 0;
    float tallest_ = 0.0f;
    std::unique_ptr<PlantPlacement> placement_;
    std::shared_ptr<const PlacementRules> rulesRef_;
    const Place* place_ = nullptr;
    double radius_ = 0.0;   // the body's the plants were placed on (0 the plane)
    Params params_;
    uint64_t generation_ = 0;
    struct Change {
        uint64_t generation;
        bromath::AABB3 bounds;
    };
    std::vector<Change> changes_;
    uint64_t changesFloor_ = 0;   // changes after this generation are all in changes_
    mutable std::vector<CellContent> cells_;
    mutable bool cellsDirty_ = false;
    mutable Stats stats_;
    std::weak_ptr<StatsSource> statsSource_;
    mutable std::vector<uint64_t> planShort_;
};

}  // namespace bro::scene
