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
//
// On a round body (world/place.h) the grids are its chart's: each cube
// face's leaves (root the face) and each layer's candidates on a grid of the
// face's metres, a candidate kept by the face its point lies on, its draws a
// hash of the face too. A candidate's fields and ground are read at its
// point on the body's datum (the 3D trees, split about the leaf's origin),
// and its record holds where it stands relative to that origin (the body's
// axes), so a place has the same plants wherever the place frame is
// anchored; PlantsNode turns them into the place frame.

#include "scene/world/fields.h"
#include "scene/world/land_grid.h"
#include "scene/world/place.h"
#include "scene/world/world_cells.h"

#include <array>
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
    std::shared_ptr<const LandBase> base;   // and a land's processes' height over it (none: the ground's)
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
/// its grid's order. On round `body` (null: the plane) x, y, z are relative
/// to placementOrigin's, in the body's axes.
std::vector<float> placeCell(const PlacementRules& rules, const CellGrid& grid, const CellKey& key,
                             const Body* body = nullptr);
/// A round body's leaf `key`'s origin: its middle's point on the datum
/// (the body's frame).
void placementOrigin(const Body& body, const CellGrid& grid, const CellKey& key, double out[3]);

class PlantPlacement {
public:
    /// A placed leaf: its key and its plants.
    struct Placed {
        CellKey key;
        std::vector<float> records;
    };

    /// On `body` (flat or null: the plane).
    PlantPlacement(std::shared_ptr<const PlacementRules> rules, const CellGrid& grid, WorldWorkers& workers,
                   const Body* body = nullptr);
    ~PlantPlacement();

    const PlacementRules& rules() const { return *rules_; }
    /// The body it places on, null on the plane.
    const Body* body() const { return round_ ? &body_ : nullptr; }

    /// The eye moved (`eye` in the body's frame: on the plane its x and z
    /// on the ground): the leaves wanted about it, the jobs posted for
    /// those not placed (`queueMs` of them at once, the nearest first),
    /// those gone too far named in `dropped`, and the jobs finished since
    /// the last call in `placed` (still wanted ones only). Never waits.
    void update(const double eye[3], std::vector<Placed>& placed, std::vector<CellKey>& dropped);

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
    /// Where the eye is on each root's chart (the plane: root 0's x, z; a
    /// round body: the faces it is near, their charts continued past their
    /// edges); a root it is not near is off.
    struct EyeOn {
        bool on = false;
        double x = 0.0, z = 0.0;
        double g[3] = {1.0, 0.0, 1.0};   // the ground's metric there (xx, xz, zz), a chart metre's
    };
    using Eyes = std::array<EyeOn, 6>;
    void eyes(const double eye[3], Eyes& out, CellKey& leaf) const;
    /// The squared distance on root k.root's chart from the eye to leaf k's
    /// square (infinite on a root the eye is off).
    double distance2(const Eyes& e, const CellKey& k) const;
    /// A root's leaf (x, z) is its own (on a round body: it reaches into
    /// the face).
    bool owns(int32_t x, int32_t z) const;
    void want(const Eyes& e);

    std::shared_ptr<const PlacementRules> rules_;
    CellGrid grid_;
    WorldWorkers& workers_;
    Body body_;
    bool round_ = false;
    double half_ = 0.0;   // a face's half-width (round)
    std::shared_ptr<Inbox> inbox_ = std::make_shared<Inbox>();
    std::unordered_set<uint64_t, KeyHash> resident_, posted_;
    std::vector<CellKey> queue_;   // wanted, not posted: the farthest first (taken from the back)
    bool looked_ = false;
    CellKey eyeLeaf_;
    Eyes wanted_;   // the eye at the last look
};

}  // namespace bro::scene
