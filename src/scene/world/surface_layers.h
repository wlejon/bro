#pragma once
// Surface layers (docs/scene-world-architecture.md §5): a surface's
// material, worked out once per tile and kept, instead of in every pixel
// that shows it. This is the CPU half, the same for every provider: which
// tiles a view needs, where they live, the fills it offers the world service
// and what the page table says. The GPU half (scene/vulkan/world/scene_surface_layers.h) owns
// the atlas and the page table and records the fills the provider draws.
//
// Tiles. A tile is a square of the ground, keyed as a world cell is
// (CellKey: root, level, x, z) on a grid of its own whose level 0 is
// `tileMetres` a side, so tiles subdivide far below a world cell's leaf; a
// tile of level L is tileMetres * 2^L metres, tileTexels texels a side plus
// a border of `border` texels baked past each edge (the bilinear filter at a
// tile's edge reads its neighbour's texels, so there is no seam).
//
// Which tiles. The same rule as everything else in the world: a texel's
// size projected from the view, in pixels. A tile is refined into its four
// children while its texel covers more than `texelPixels` pixels at the
// tile's nearest point. A view down onto the ground and a view across it
// differ: across, a pixel's footprint is stretched along the view by one
// over the sine of the angle the view meets the ground at, so a tile's
// footprint estimate divides by the largest sine its box can show (the
// shader takes its level from the same footprint, length(fwidth(rel))); the
// heights and the slope are the ground's under the tile (Items below). The
// tiles chosen are a quadtree's leaves and every one of their ancestors, so
// a fragment always finds its level and the next coarser one, and the
// coarsest level's tiles are always kept. The atlas's slots bound them: the
// tile most too coarse (its texel over the pixels it should cover) is
// refined first, so a view that wants more than there is room for falls
// short evenly, worst first, rather than near the eye or far from it
// (stats().worstError says by how much). Nothing is read back from the GPU.
//
// Slots and the page table. A tile lives in one of `slots` atlas slots (an
// array layer per slot). The page table is one grid of pageSize^2 entries a
// level; tile (x, z) of level L is entry (x mod pageSize, z mod pageSize) of
// layer L, which holds its slot + 1 and the tile's x and z above the grid's
// own bits (so a tile pageSize away that shares the entry is told apart):
// encodePage below, ground/scene_ground_layers.glsl's lookup. Two tiles that would
// share an entry are never paged at once (the further one goes).
//
// Items. The provider describes its data by where it lies: items, each a box
// (absolute world) and a generation. A tile's box takes its heights from the
// items under it (so the footprint above sees how steeply the view meets
// that ground, and the frustum test the ground's real extent), a tile with
// no item under it is never needed, and a tile's generation is the
// provider's own (setGeneration: what every tile reads) mixed with those of
// the items under it. A change to one item stales only the tiles over it.
// With no items, the bounds are one item of generation 0.
//
// Filling. Each frame the tiles a view needs but has not got (or got at an
// older generation of the provider's data) are the layer's jobs (jobs()),
// which the world service (world_service.h, docs/scene-world-architecture.md
// §6) ranks against every other producer's and grants as the frame's slack
// allows; the granted ones are filled (commit()). A job's error is in
// pixels, what the view shows while it is not made: a missing tile's texels
// are its nearest resident ancestor's, 2^(levels between) times its own on
// screen; a stale tile's are its own. A tile's error is never above its
// parent's while that one is a job too, so with ties going to the larger
// tile a tile is never ranked ahead of its parent. The coarsest level's
// missing tiles are required (filled whatever the budget), so nothing is
// ever without a layer after the first frame. A stale tile keeps drawing
// until its refill lands. A tile no view needs stays where it is until its
// slot is wanted (least recently needed first), so turning back finds it. A
// slot is reused in the same command stream that read it, after it, so
// nothing waits for a reader to finish.

#include "scene/world/world_cells.h"

#include <bromath/aabb.h>
#include <bromath/frustum.h>
#include <bromath/vec.h>

#include <cstdint>
#include <span>
#include <unordered_map>
#include <utility>
#include <vector>

namespace bro::scene {

struct SurfaceLayerConfig {
    float tileMetres = 1.0f;     // a level-0 tile's side
    int tileTexels = 64;         // a tile's texels a side, inside its border
    int border = 1;              // texels past each edge
    int slots = 1024;            // atlas slots (at most kMaxSlots)
    int pageSize = 256;          // page-table entries a side, a level (a power of two)
    int maxLevels = 20;          // the most levels (the provider's bounds set how many)
    float texelPixels = 1.5f;    // a tile is refined while its texel covers more pixels than this

    int stride() const { return tileTexels + 2 * border; }
};

/// A tile the view needs and has not got at the current generation: a job
/// for the world service (WorldJob's fields, the layer's side of them).
struct SurfaceJob {
    CellKey key;
    float error = 0.0f;      // pixels on screen while it is not filled
    double side = 1.0;       // metres
    bool required = false;   // a missing tile of the coarsest level
};

/// A tile to fill this frame: its key, its slot and its ground rectangle
/// without the border (x0, z0, side; the border reaches `border` texels
/// past every edge).
struct SurfaceFill {
    CellKey key;
    int slot = 0;
    double x0 = 0.0, z0 = 0.0, side = 1.0;
};

/// One page-table entry to write: layer `level`, entry (ex, ez).
struct SurfacePageWrite {
    int level = 0, ex = 0, ez = 0;
    uint32_t value = 0;
};

/// A piece of the provider's data: where it lies (absolute world) and its
/// generation (anything that changes what a fill over it would write).
struct SurfaceItem {
    bromath::AABB3 bounds;
    uint64_t generation = 0;
};

struct SurfaceLayerStats {
    uint32_t needed = 0;      // tiles the view needs
    uint32_t resident = 0;    // tiles holding a slot
    uint32_t missing = 0;     // needed and not filled at the current generation (after commit)
    uint32_t filled = 0;      // fills issued this frame
    uint32_t levels = 0;
    /// The atlas's shortfall: how many times too coarse the worst tile the
    /// slots left unrefined is (1: none; the view has every tile it wants).
    float worstError = 1.0f;
};

class SurfaceLayers {
public:
    static constexpr int kMaxSlots = 4095;

    /// What a view gives the selection: its eye, frustum (absolute world)
    /// and how many metres a pixel covers a metre away (1 / focal length in
    /// pixels); an orthographic view has `orthoMetresPerPixel` instead.
    struct View {
        bromath::Vec3 eye{0.0f, 0.0f, 0.0f};
        bromath::Frustum frustum;
        float focalPixels = 1000.0f;
        bool perspective = true;
        float orthoMetresPerPixel = 0.01f;
    };

    /// (Re)start with `config` over the provider's `bounds` (absolute world:
    /// its x/z extent decides the levels, its y range bounds every tile's
    /// box). Drops every tile.
    void configure(const SurfaceLayerConfig& config, const bromath::AABB3& bounds);
    /// The provider's bounds now: kept tiles stay unless the extent needs
    /// more levels than configured, which starts over (true: every tile went).
    bool setBounds(const bromath::AABB3& bounds);
    bool configured() const { return levels_ > 0; }
    const SurfaceLayerConfig& config() const { return config_; }
    int levels() const { return levels_; }
    const bromath::AABB3& bounds() const { return bounds_; }

    /// The provider's data generation, what every tile reads: tiles filled
    /// at another are stale.
    void setGeneration(uint64_t generation) { generation_ = generation; }
    uint64_t generation() const { return generation_; }
    /// The provider's items (see Items above); replaces the last. They are
    /// gathered into a pyramid of cells (each cell's lowest and highest
    /// ground and its items' generations), so a tile reads the few cells
    /// under it whatever the items number: an item finer than a cell
    /// (gridCells a side over the extent) is as coarse as its cell.
    void setItems(std::vector<SurfaceItem> items) {
        items_ = std::move(items);
        gridDirty_ = true;
    }
    static constexpr int kGridCells = 512;

    /// Select the tiles `view` needs and gather the jobs (jobs()): those not
    /// filled at the current generation. `frame` counts up by one a call.
    void update(const View& view, uint64_t frame);
    const std::vector<SurfaceJob>& jobs() const { return jobs_; }
    /// Fill the granted jobs (their keys' packed bits, in the order given):
    /// give them slots (fills()) and gather the page-table writes
    /// (pageWrites(), to apply after the fills). Once a frame, after update(),
    /// with no keys when none were granted (it settles the frame's stats).
    void commit(std::span<const uint64_t> granted);

    const std::vector<SurfaceFill>& fills() const { return fills_; }
    const std::vector<SurfacePageWrite>& pageWrites() const { return pageWrites_; }
    const SurfaceLayerStats& stats() const { return stats_; }

    /// A tile's texel size, metres.
    float texelMetres(int level) const;
    /// The page-table word for a tile in `slot` (ground/scene_ground_layers.glsl's).
    static uint32_t encodePage(const CellKey& key, int slot, int pageShift);
    int pageShift() const { return pageShift_; }

private:
    struct Tile {
        CellKey key;
        int slot = -1;
        uint64_t generation = 0;   // what it was filled at
        uint64_t wanted = 0;       // what the view that last needed it wanted
        uint64_t lastNeeded = 0;
        bool filled = false;
    };
    struct Need {
        CellKey key;
        float distance = 0.0f;
        uint64_t generation = 0;
        float texelPixels = 0.0f;   // its texel's pixels on screen at its nearest
    };

    struct Cell {
        float lo, hi;     // lo > hi: no ground
        uint64_t sum;     // its items' generations, mixed
        float span = 0.0f;   // (under's) the cells' width, metres
    };
    void buildGrid();
    /// The ground under tile `key` with `pad` metres past its edges.
    Cell under(const CellKey& key, double pad) const;
    /// The tiles `view` needs (within the slots); returns the worst error
    /// left unrefined.
    float select(const View& view, std::vector<Need>& out) const;
    int takeSlot(uint64_t frame);
    void evict(uint64_t packed);
    void page(const Tile& tile);
    uint32_t pageIndex(const CellKey& key) const;

    SurfaceLayerConfig config_;
    bromath::AABB3 bounds_ = bromath::aempty3();
    int levels_ = 0;
    int pageShift_ = 8;
    uint64_t generation_ = 0;
    std::vector<SurfaceItem> items_;
    bool gridDirty_ = true;
    int gridShift_ = 0;            // a base cell is a level-gridShift_ tile
    int gridX0_ = 0, gridZ0_ = 0;  // the base cells' first (in base cells)
    std::vector<int> gridSide_;    // cells a side, each pyramid level
    std::vector<std::vector<Cell>> grid_;

    std::unordered_map<uint64_t, Tile> tiles_;   // by CellKey::packed
    std::vector<uint64_t> slotTile_;              // each slot's tile (kFree when none)
    std::vector<int> freeSlots_;
    std::vector<uint64_t> pageTile_;              // each page entry's tile (kFree when none)

    std::vector<SurfaceJob> jobs_;
    uint64_t frame_ = 0;   // update()'s
    std::vector<SurfaceFill> fills_;
    std::vector<SurfacePageWrite> pageWrites_;
    SurfaceLayerStats stats_;
};

}  // namespace bro::scene
