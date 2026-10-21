#pragma once
// Placement (docs/scene-world-architecture.md §10): a world's plants made
// where the eye is, from its fields and its rules, rather than stored.
//
// The rules are data: a ground field (heights) and layers, each a kind of
// planting (the canopy's trees, the understorey's shrubs). A layer lays
// candidates on a square grid of its spacing over the whole world, each
// moved by up to `jitter` of the spacing; a candidate grows where its draw
// is under the layer's chance field, as a species drawn by the species'
// weight fields there, its size and tint from their fields with a little of
// its own. Every draw is a hash of the layer's seed and the candidate's
// place on its grid, never of the cell or the order of work, so a place has
// the same plants however and whenever it is made, and a cell's plants are
// the candidates whose grid points are in it.
//
// PlantPlacement keeps a node's cells placed about the eye: the leaves
// within `range` are wanted, nearest first, each placed by a job on the
// world's workers (placeCell); one past range + hysteresis is dropped. It
// never waits: a frame takes what jobs have finished (update), the rest
// arrive later.

#include "scene/world/fields.h"
#include "scene/world/world_cells.h"

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace bro::scene {

class WorldWorkers;

struct PlacementLayer {
    float spacing = 5.0f;   // between candidates, metres
    float jitter = 0.9f;    // of the spacing: a candidate moves up to half of it each way
    FieldRef chance;        // where a candidate grows: its draw under this (none: every one)
    struct Choice {
        int species = 0;
        FieldRef weight;    // its share where it is (none: 1)
    };
    std::vector<Choice> species;
    FieldRef scale;         // a plant's size (none: 1) ...
    float scaleVariety = 0.0f;   // ... times 1 + this of its own, either way
    FieldRef tint;          // its tint (none: 0) ...
    float tintVariety = 0.0f;    // ... plus up to half this of its own, either way
    float sink = 0.0f;      // how far its foot stands into the ground
    uint32_t seed = 1;      // its draws'
};

struct PlacementRules {
    FieldRef ground;        // the ground's height (none: 0)
    std::vector<PlacementLayer> layers;
    float range = 1000.0f;  // the leaves within this of the eye (on the ground) are placed
    float hysteresis = 128.0f;   // and kept until this farther
};

/// Rules from their JSON (what js/scene_plants.js sends): its fields a
/// table, {"tree": encoded, "seed": n}, each rule's field an index into it:
///   {"fields": [...], "ground": i, "range": m, "hysteresis": m,
///    "layers": [{"spacing", "jitter", "chance": i,
///                "species": [{"species": s, "weight": i}],
///                "scale": i, "scaleVariety", "tint": i, "tintVariety",
///                "sink", "seed"}]}
/// A field left out or null is none. False, with a message, on JSON that
/// is not this, or a tree that does not decode.
bool parsePlacementRules(const std::string& json, PlacementRules& out, std::string* error = nullptr);

/// The plants of leaf `key`, PlantsNode's records (kPlantFloats each:
/// x, y, z, yaw, scale, species, seed, tint), layer by layer, a layer's in
/// its grid's order.
std::vector<float> placeCell(const PlacementRules& rules, const CellGrid& grid, const CellKey& key);

class PlantPlacement {
public:
    /// A placed leaf: its key and its plants.
    struct Placed {
        CellKey key;
        std::vector<float> records;
    };

    PlantPlacement(std::shared_ptr<const PlacementRules> rules, const CellGrid& grid, WorldWorkers& workers);
    ~PlantPlacement();

    const PlacementRules& rules() const { return *rules_; }

    /// The eye moved (on the ground, x and z): the leaves wanted about it,
    /// the jobs posted for those not placed (`queueMs` of them at once, the
    /// nearest first), those gone too far named in `dropped`, and the
    /// jobs finished since the last call in `placed` (still wanted ones
    /// only). Never waits.
    void update(double eyeX, double eyeZ, std::vector<Placed>& placed, std::vector<CellKey>& dropped);

    /// Leaves placed (handed out by update and not dropped since).
    size_t resident() const { return resident_.size(); }
    /// Leaves wanted and not yet placed (posted or waiting); before the
    /// first update, 1 (the look about the eye still to come).
    size_t pending() const;
    /// The work kept posted, in ms of each pool thread's time at the jobs'
    /// cost learned as they run: enough to keep the pool busy from one
    /// update to the next, little enough that a turn of the eye soon
    /// re-ranks what is left. 0 posts one job at a time.
    double queueMs = 4.0;

private:
    struct Inbox {
        std::mutex mutex;
        std::vector<Placed> done;
        double costMs = 0.0;   // a job's, smoothed (0 before the first)
    };
    struct KeyHash {
        size_t operator()(uint64_t k) const { return std::hash<uint64_t>()(k); }
    };
    void want(double eyeX, double eyeZ);

    std::shared_ptr<const PlacementRules> rules_;
    CellGrid grid_;
    WorldWorkers& workers_;
    std::shared_ptr<Inbox> inbox_ = std::make_shared<Inbox>();
    std::unordered_set<uint64_t, KeyHash> resident_, posted_;
    std::vector<CellKey> queue_;   // wanted, not posted: the farthest first (taken from the back)
    int32_t eyeLeafX_ = INT32_MIN, eyeLeafZ_ = INT32_MIN;
    double wantX_ = 1e30, wantZ_ = 1e30;   // the eye at the last look
};

}  // namespace bro::scene
