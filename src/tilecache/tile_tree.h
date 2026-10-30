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

    /// Up to `n` missing wanted tiles to produce now, each with a slot, in
    /// order (a tile's parent always before it). Each must then be
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
    /// it and its descendants. Kept while the tree lives (cleared by clear()).
    void setRange(const TileId& id, HeightRange r);
    /// The slot of a ready tile, or -1.
    int32_t slotOf(const TileId& id) const;

    /// Forget every tile (a world's producer changed): all slots free.
    void clear();

private:
    enum class State : uint8_t { Missing, Producing, Ready };
    struct Entry {
        TileId id;
        int32_t slot = -1;
        State state = State::Missing;
        uint64_t lastWanted = 0;
    };
    struct Wanted {
        TileId id;
        double error = 0;   // > 1: its cells project larger than pixelsPerCell
        bool leaf = true;
        bool visible = true;
    };

    // A tile's base footprint (fixed) and the range last resolved for it.
    struct Footprint {
        Vec3d center;        // base surface at (0.5, 0.5)
        Vec3d normal;        // the base normal there
        double radius = 0;   // the farthest base point from center
        double sag = 0;      // how far the base falls below its tangent plane at center
        HeightRange range;
        uint64_t rangeGen = 0;
    };
    const Footprint& footprint(const TileId& id) const;
    /// The tight sphere selection and culling use: about the middle of the
    /// tile's range rather than its base.
    Bounds tightBounds(const Footprint& f) const;
    /// `top`: the tile's highest surface above its base (its range's hi).
    bool visible(const Bounds& b, double top, const View& view) const;
    /// 0 for a tile the view culls.
    double error(const TileId& id, const View& view, bool* seen) const;
    bool isReady(const TileId& id) const;
    bool allocate(Entry& e);
    void coverInto(const TileId& id, std::vector<DrawTile>& out, bool& ok) const;

    Config config_;
    uint64_t frame_ = 0;
    std::unordered_map<uint64_t, Entry> entries_;
    std::unordered_map<uint64_t, Wanted> wanted_;
    std::vector<uint32_t> freeSlots_;
    mutable std::unordered_map<uint64_t, Footprint> footprints_;
    std::unordered_map<uint64_t, HeightRange> ranges_;
    uint64_t rangeGen_ = 1;   // advanced by every setRange: resolved ranges are stale
    Stats stats_;
};

} // namespace bro::tilecache
