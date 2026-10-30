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

struct Config {
    Domain domain;
    uint32_t tileSize = 64;      // cells along a tile's edge (borders not counted)
    uint32_t capacity = 256;     // slots: the most tiles resident at once
    uint32_t maxLevel = 16;
    double maxHeight = 0;        // how far the surface may leave its base, for bounds
    double pixelsPerCell = 2;    // split while a cell projects larger than this
};

struct View {
    Vec3d position;
    /// Pixels per unit of size at unit distance: viewport height / (2 tan(fov / 2)).
    double projScale = 1;
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

    /// The ready tiles that cover the wanted tree without overlap.
    std::vector<DrawTile> drawSet() const;

    /// Wanted tiles not ready yet.
    uint32_t pending() const;
    uint32_t wanted() const { return static_cast<uint32_t>(wanted_.size()); }
    uint32_t resident() const { return config_.capacity - static_cast<uint32_t>(freeSlots_.size()); }
    const Stats& stats() const { return stats_; }

    Bounds bounds(const TileId& id) const;
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
    };

    double error(const TileId& id, const View& view) const;
    bool isReady(const TileId& id) const;
    bool allocate(Entry& e);
    void coverInto(const TileId& id, std::vector<DrawTile>& out, bool& ok) const;

    Config config_;
    uint64_t frame_ = 0;
    std::unordered_map<uint64_t, Entry> entries_;
    std::unordered_map<uint64_t, Wanted> wanted_;
    std::vector<uint32_t> freeSlots_;
    Stats stats_;
};

} // namespace bro::tilecache
