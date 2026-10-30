#pragma once

// TileTree: the CPU half of a tile cache, the engine system world code fills
// on demand (bro.tiles, docs/tiles-api.js). Proland's producer/cache design:
//
//   - A domain is a forest of quadtrees: one root for a plane (or a grid of
//     them), six for a cube-sphere. A tile is (root, level, x, y), x and y
//     below 2^level, and covers a quarter of its parent.
//   - select() picks the tiles a view wants: from the roots, the tile whose
//     cells project largest on screen is split first, until every leaf's
//     cells project under `pixelsPerCell` or the wanted tree fills the cache
//     (Nanite's error-driven choice, rather than distance bands).
//   - Every wanted tile has a slot in the GPU pools; the parents of wanted
//     tiles are wanted too, so a child can always be derived from its parent
//     (refinement: a world's producer reads the parent's slot) and a draw can
//     blend toward it.
//   - takeProductions() hands out the missing tiles whose parent is ready,
//     coarse first, then by projected size; a slot comes from the free list,
//     else from the least recently wanted tile no view wants now.
//   - drawSet() covers each root with ready tiles and never overlaps: a
//     wanted tile whose children are not all ready yet draws itself.
//
// No GPU here: the caller owns the pools and runs the producers.

#include <cstdint>
#include <unordered_map>
#include <vector>

namespace bro::tilecache {

struct TileId {
    uint32_t root = 0;
    uint32_t level = 0;
    uint32_t x = 0;
    uint32_t y = 0;

    static constexpr uint32_t kMaxLevel = 24;

    TileId parent() const { return {root, level - 1, x >> 1, y >> 1}; }
    /// Child 0..3: (x, y), (x+1, y), (x, y+1), (x+1, y+1) at level + 1.
    TileId child(uint32_t i) const { return {root, level + 1, 2 * x + (i & 1), 2 * y + (i >> 1)}; }
    uint64_t key() const {
        return (static_cast<uint64_t>(root) << 56) | (static_cast<uint64_t>(level) << 50) |
               (static_cast<uint64_t>(x) << 25) | static_cast<uint64_t>(y);
    }
    bool operator==(const TileId& o) const { return key() == o.key(); }
};

struct Vec3d {
    double x = 0, y = 0, z = 0;
};

/// Where a tile is in the world: its base surface, before any height.
struct Domain {
    enum class Kind { Plane, CubeSphere };
    Kind kind = Kind::Plane;
    // Plane (y up): rootsX x rootsY square roots of edge `size`, root r at
    // column r % rootsX, row r / rootsX, its (u, v) along +x and +z from
    // `origin` (x, z).
    double origin[2] = {0, 0};
    double size = 1;
    uint32_t rootsX = 1, rootsY = 1;
    // CubeSphere: six roots, the cube's faces pushed onto a sphere of
    // `radius` about `center` (point()).
    double radius = 1;
    Vec3d center;

    uint32_t rootCount() const { return kind == Kind::Plane ? rootsX * rootsY : 6; }

    /// A tile as a parallelogram in base coordinates: base(u, v) = corner +
    /// u * du + v * dv for (u, v) in [0, 1]^2. A plane's base coordinates are
    /// the world's; a cube-sphere's are the cube's ([-1, 1]^3), and the world
    /// point is center + normalize(base) * radius.
    struct Frame {
        Vec3d corner, du, dv;
    };
    Frame frame(const TileId& t) const;
    /// The base surface at (u, v) in [0, 1]^2 of the tile.
    Vec3d point(const TileId& t, double u, double v) const;
    /// A tile's edge length on the base surface (a cube-sphere face spans a
    /// quarter of a great circle).
    double tileExtent(const TileId& t) const;
};

struct Bounds {
    Vec3d center;
    double radius = 0;
};

/// How far the surface within a tile leaves its base, along the base normal:
/// at this tile's level and every finer one (a child's detail stays inside
/// its parent's range).
struct HeightRange {
    double lo = 0, hi = 0;
};

struct Config {
    Domain domain;
    uint32_t tileSize = 64;      // cells along a tile's edge (borders not counted)
    uint32_t capacity = 256;     // slots: the most tiles resident at once
    uint32_t maxLevel = 16;
    // The range every tile's surface lies in until one is set for it or an
    // ancestor (setRange): what bounds it for selection.
    double minHeight = 0;
    double maxHeight = 0;
    double pixelsPerCell = 2;    // split while a cell projects larger than this
};

struct View {
    Vec3d position;
    /// Pixels per unit of size at unit distance: viewport height / (2 tan(fov / 2)).
    double projScale = 1;
    /// With a frustum, a tile outside it (or, on a cube-sphere, below the
    /// horizon) is not refined and not drawn; it stays wanted at the level
    /// that covers it. `forward`, `right` and `up` are unit and orthogonal;
    /// tanHalfX / tanHalfY are the tangents of the half field of view.
    bool frustum = false;
    Vec3d forward, right, up;
    double tanHalfX = 1, tanHalfY = 1;
};

struct Production {
    TileId id;
    uint32_t slot = 0;
    int32_t parentSlot = -1;     // -1 for a root
};

struct DrawTile {
    TileId id;
    uint32_t slot = 0;
    int32_t parentSlot = -1;     // the parent's slot when it is resident, else -1
    Bounds bounds;
};

struct Stats {
    uint64_t produced = 0;
    uint64_t evicted = 0;
};

class TileTree {
public:
    explicit TileTree(const Config& config);

    const Config& config() const { return config_; }

    /// Pick the tiles `view` wants (replacing the previous choice).
    void select(const View& view);

    /// Up to `n` missing wanted tiles to produce now, each with a slot, each
    /// one's parent ready (never in the same batch). Each must then be
    /// markReady()'d or abandon()'d.
    std::vector<Production> takeProductions(uint32_t n);
    void markReady(const TileId& id);
    /// The production failed: the slot goes back, the tile stays missing.
    void abandon(const TileId& id);

    /// The ready tiles that cover the wanted tree without overlap, less those
    /// the last view culled.
    std::vector<DrawTile> drawSet() const;

    /// Wanted tiles not ready yet.
    uint32_t pending() const;
    uint32_t wanted() const { return static_cast<uint32_t>(wanted_.size()); }
    /// Tiles the tree keeps nodes for (wanted, resident, ranged, or above
    /// one that is): bounded by a few capacities.
    uint32_t known() const { return static_cast<uint32_t>(index_.size()); }
    uint32_t resident() const { return config_.capacity - static_cast<uint32_t>(freeSlots_.size()); }
    const Stats& stats() const { return stats_; }

    /// A sphere about the tile's base center (its base surface at (0.5, 0.5),
    /// fixed for the tile) holding its surface within range(id).
    Bounds bounds(const TileId& id) const;
    /// The tile's height range: its own once set, else its nearest
    /// ancestor's, else [minHeight, maxHeight].
    HeightRange range(const TileId& id) const;
    /// What the surface of `id` and every tile below it lies within, as its
    /// producer measured it: narrows the bounds selection and culling use for
    /// it and its descendants from the next select. Kept while the tree keeps
    /// the tile (cleared by clear(); a tile pruned once the tree outgrows a
    /// few capacities falls back to its ancestor's).
    void setRange(const TileId& id, HeightRange r);
    /// The slot of a ready tile, or -1.
    int32_t slotOf(const TileId& id) const;

    /// Forget every tile (a world's producer changed): all slots free.
    void clear();

private:
    enum class State : uint8_t { Missing, Producing, Ready };

    // A tile the tree knows: wanted, resident, holding a range, or above one
    // that is. Nodes link to their parent and children by index, so selection
    // and the draw set walk the tree without a lookup; `index_` finds a node
    // by id for the calls that name one.
    struct Node {
        TileId id;
        int32_t parent = -1;
        int32_t child[4] = {-1, -1, -1, -1};
        // The base footprint, fixed for the tile.
        Vec3d center;        // base surface at (0.5, 0.5)
        Vec3d normal;        // the base normal there
        double radius = 0;   // the farthest base point from center
        double sag = 0;      // how far the base falls below its tangent plane at center
        double cell = 0;     // a cell's edge on the base surface
        // Its own range once set; `range` is the one it is selected by (its
        // own, else its parent's), resolved top-down as selection reaches it.
        HeightRange own;
        bool hasOwn = false;
        HeightRange range;
        // Residency.
        int32_t slot = -1;
        State state = State::Missing;
        uint64_t lastWanted = 0;   // == frame_: wanted by the last select
        // The last select's verdict.
        double error = 0;    // > 1: its cells project larger than pixelsPerCell
        bool leaf = true;
        bool visible = true;
        bool live = false;   // false: on the free list
    };

    void footprintOf(Node& n) const;
    int32_t find(const TileId& id) const;
    /// The node for `id`, made (with its ancestors) if the tree lacks it.
    int32_t ensure(const TileId& id);
    int32_t ensureChild(int32_t parent, uint32_t i);
    int32_t makeNode(const TileId& id, int32_t parent);
    /// range from the node's own, else its parent's resolved range.
    void resolve(Node& n) const;
    /// Drops the nodes nothing needs (not wanted, not resident, no child
    /// kept) once the tree holds far more than its capacity.
    void prune();
    Bounds bounds(const Node& n, const HeightRange& r) const;
    /// The tight sphere selection and culling use: about the middle of the
    /// tile's range rather than its base.
    Bounds tightBounds(const Node& n) const;
    // What culling needs of a view, worked out once a select.
    struct Cull {
        double cx = 1, cy = 1;      // the side planes' unit-normal scales
        bool horizon = false;       // the eye is above the lowest surface
        double r0 = 0;              // that surface's radius
        double eyeReach = 0;        // the eye's tangent length to it
    };
    Cull cullFor(const View& view) const;
    /// `top`: the tile's highest surface above its base (its range's hi).
    bool visible(const Bounds& b, double top, const View& view, const Cull& cull) const;
    /// 0 for a tile the view culls.
    double error(const Node& n, const View& view, const Cull& cull, bool* seen) const;
    bool wantedNow(const Node& n) const { return n.lastWanted == frame_; }
    void evict(Node& n);
    void coverInto(int32_t node, std::vector<DrawTile>& out, bool& ok) const;

    Config config_;
    uint64_t frame_ = 0;
    std::vector<Node> nodes_;
    std::vector<int32_t> freeNodes_;
    std::unordered_map<uint64_t, int32_t> index_;
    std::vector<int32_t> wanted_;     // the last select's tiles, in the order it chose them
    std::vector<int32_t> slotNode_;   // the node holding each slot, or -1
    std::vector<uint32_t> freeSlots_;
    struct Open {
        double error;
        uint64_t key;
        int32_t node;
    };
    std::vector<Open> open_;          // select's heap, kept for its storage
    size_t pruneCursor_ = 0;          // where prune()'s next sweep starts
    Stats stats_;
};

} // namespace bro::tilecache
