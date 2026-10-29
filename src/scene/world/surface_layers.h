#pragma once
// Surface layers: a surface's
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
// Roots and the map. A tile is a square of a root's chart (place.h): a flat
// world's one root is its plane's x and z, and with no map that square is
// the ground's own box in the views' frame. A round body's six cube faces
// are six roots, and its map (SurfaceMap) gives each square's box in the
// views' frame, how high the eye stands over it and whether the body hides
// it; everything below is the same for both.
//
// Items. The provider describes its data by where it lies: items, each a box
// (in its root's chart: x and z the chart's, y the ground's heights) and a
// generation. A tile's box takes its heights from the
// items under it (so the footprint above sees how steeply the view meets
// that ground, and the frustum test the ground's real extent), a tile with
// no item under it is never needed, and a tile's generation is the
// provider's own (setGeneration: what every tile reads) mixed with those of
// the items under it. A change to one item stales only the tiles over it.
// With no items, the bounds are one item of generation 0.
//
// Filling. Each frame the tiles a view needs but has not got (or got at an
// older generation of the provider's data) are the layer's jobs (jobs()),
// which the world service (world_service.h) ranks against every other producer's and grants as the frame's slack
// allows; the granted ones are filled (commit()). A job's error is in
// pixels, what the view shows while it is not made: a missing tile's texels
// are its nearest resident ancestor's, 2^(levels between) times its own on
// screen (none resident: the coarsest's, required and so made that frame;
// a missing coarsest tile, as if one above it), at most about the view's
// height (twice its focal length in pixels); a stale tile's are its own; a
// tile the view sees only through children it holds, no more than
// texelPixels (drawn nowhere, its children's jobs say what it shows where
// one is missing). A tile's error is never above its
// parent's (what that one shows) while that one is a job too, so with ties
// going to the larger tile a tile is never ranked ahead of a parent it
// shows through. The coarsest level's
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
    // Out of the view, within shadeReach of the eye, where its shade (away
    // from the view's sun, as far as its own heights could cast it) falls
    // into the view: a tile is refined while
    // its texel is wider than its distance over shadeTexels (no finer than
    // shadeTexelMetres), what a reader of the ground about the eye needs of
    // it where the view does not look; as wrong, it counts texelPixels
    // pixels a texel. 0: such tiles are only the coarsest.
    float shadeTexels = 0.0f;
    float shadeTexelMetres = 0.0f;
    float shadeReach = 0.0f;

    int stride() const { return tileTexels + 2 * border; }
};

/// A tile the view needs and has not got at the current generation: a job
/// for the world service (WorldJob's fields, the layer's side of them).
struct SurfaceJob {
    CellKey key;
    float error = 0.0f;      // pixels on screen while it is not filled
    double side = 1.0;       // metres
    bool required = false;   // a missing tile of the coarsest level
    bool stale = false;      // held, but filled from items since changed
    float distance = 0.0f;   // from the eye to its nearest (metres)
};

/// A tile to fill this frame: its key, its slot and its ground rectangle
/// without the border (x0, z0, side; the border reaches `border` texels
/// past every edge).
struct SurfaceFill {
    CellKey key;
    int slot = 0;
    double x0 = 0.0, z0 = 0.0, side = 1.0;
};

/// One page-table entry to write: array layer `layer` (a root's levels
/// together: root * maxLevels + level), entry (ex, ez).
struct SurfacePageWrite {
    int layer = 0, ex = 0, ez = 0;
    uint32_t value = 0;
};

/// A piece of the provider's data: where it lies (its root's chart, its
/// heights) and its generation (anything that changes what a fill over it
/// would write).
struct SurfaceItem {
    bromath::AABB3 bounds;
    uint64_t generation = 0;
    uint8_t root = 0;
};

/// Where a root's chart squares lie in the views' frame (Roots above).
class SurfaceMap {
public:
    struct Patch {
        bromath::AABB3 box;    // the square's ground between its heights
        float above = 0.0f;    // the eye's height over its ground (at the heights' middle)
        float nearest = 0.0f;  // no ground of it nearer the eye (where the box says less)
        bool hidden = false;   // the body hides all of it from the eye
    };
    virtual ~SurfaceMap() = default;
    virtual int roots() const = 0;
    /// Root `root`'s square [x0, x1] x [z0, z1] with its ground between
    /// heights lo and hi, seen from `eye`.
    virtual Patch patch(int root, double x0, double z0, double x1, double z1, float lo, float hi,
                        const bromath::Vec3& eye) const = 0;
};

struct SurfaceLayerStats {
    uint32_t needed = 0;      // tiles the view needs
    uint32_t resident = 0;    // tiles holding a slot
    uint32_t missing = 0;     // needed and not filled at the current generation (after commit)
    uint32_t filled = 0;      // fills issued this frame
    uint32_t levels = 0;
    uint32_t gridCells = 0;   // base cells re-gathered this frame (the items' changes; all of them on a rebuild)
    /// The atlas's shortfall: how many times too coarse the worst tile the
    /// slots left unrefined is (1: none; the view has every tile it wants).
    float worstError = 1.0f;
};

class SurfaceLayers {
public:
    static constexpr int kMaxSlots = 4095;

    /// What a view gives the selection: its eye, frustum (the views' frame)
    /// and how many metres a pixel covers a metre away (1 / focal length in
    /// pixels); an orthographic view has `orthoMetresPerPixel` instead.
    struct View {
        bromath::Vec3 eye{0.0f, 0.0f, 0.0f};
        bromath::Frustum frustum;
        float focalPixels = 1000.0f;
        bool perspective = true;
        float orthoMetresPerPixel = 0.01f;
        bromath::Vec3 shade{0.0f, 0.0f, 0.0f};   // toward the sun the shade reader casts by (none: zero)
    };

    /// (Re)start with `config` over the provider's `bounds` (every root's
    /// chart: its x/z extent decides the levels, its y range bounds every
    /// tile's box) and `map` (none: the plane's, one root; it must outlive
    /// its use). Drops every tile.
    void configure(const SurfaceLayerConfig& config, const bromath::AABB3& bounds, const SurfaceMap* map = nullptr);
    int roots() const { return map_ ? map_->roots() : 1; }
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
    /// under it whatever the items number: an item finer than a cell is as
    /// coarse as its cell. A base cell is a tile of one level, at most
    /// kGridCells of them across a coarsest tile and at most kGridBudget of
    /// them over every root together (a round body's six faces, twenty
    /// coarsest tiles a side, get 320 a face), so the grid's memory is
    /// bounded whatever the bounds. Items are told apart by their index: the
    /// next update() re-gathers only the cells under the items that changed
    /// (their old and new footprints) and those cells' ancestors.
    void setItems(std::vector<SurfaceItem> items) {
        items_ = std::move(items);
        itemsDirty_ = true;
    }
    static constexpr int kGridCells = 512;
    static constexpr size_t kGridBudget = size_t{1} << 20;
    /// The grid's bytes now (cells, the cells' item lists, the items kept).
    size_t gridBytes() const;

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
    /// The page table's array layer of a tile: its root's levels together
    /// (root * maxLevels + level), so the table has maxLevels * roots layers.
    int pageLayer(const CellKey& key) const { return key.root * config_.maxLevels + key.level; }

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
    };
    /// What under() finds: the cells' ground and their width, metres.
    struct Ground : Cell {
        float span = 0.0f;
    };
    /// A root's pyramid of cells (buildGrid).
    struct Grid {
        std::vector<int> side;   // cells a side, each pyramid level
        std::vector<std::vector<Cell>> cells;
        std::vector<int32_t> head;   // each base cell's first link (-1: none)
    };
    /// One item over one base cell, in that cell's list.
    struct Link {
        uint32_t item;
        int32_t next;
    };
    /// Where the grid's base cells lie: level-shift tiles from (x0, z0), n a side.
    struct GridLayout {
        int shift = 0, x0 = 0, z0 = 0, n = 0;
        bool operator==(const GridLayout&) const = default;
    };
    GridLayout gridLayout() const;
    void buildGrid(const GridLayout& layout);
    /// A base cell's ground from its list of items (from link `node`).
    Cell gather(int32_t node) const;
    /// Bring the grid to items_ (or the bounds' whole item), re-gathering
    /// only the cells the changed items lie over; false when none changed.
    bool updateGrid();
    /// The base cells item `item` lies over (empty: i0 > i1).
    bool footprint(const SurfaceItem& item, int& i0, int& i1, int& j0, int& j1) const;
    void link(uint32_t k, const SurfaceItem& item, std::vector<uint64_t>* dirty);
    void unlink(uint32_t k, const SurfaceItem& item, std::vector<uint64_t>& dirty);
    /// The ground under tile `key` with `pad` metres past its edges.
    Ground under(const CellKey& key, double pad) const;
    /// The tiles `view` needs (within the slots); returns the worst error
    /// left unrefined.
    float select(const View& view, std::vector<Need>& out) const;
    int takeSlot(uint64_t frame);
    void evict(uint64_t packed);
    void page(const Tile& tile);
    uint32_t pageIndex(const CellKey& key) const;

    SurfaceLayerConfig config_;
    bromath::AABB3 bounds_ = bromath::aempty3();
    const SurfaceMap* map_ = nullptr;
    int levels_ = 0;
    int pageShift_ = 8;
    uint64_t generation_ = 0;
    std::vector<SurfaceItem> items_;   // the provider's, until the next update() takes them
    bool itemsDirty_ = true;
    bool ownItems_ = false;            // the provider gave items (else the bounds are the one)
    bool gridDirty_ = true;        // rebuild it all (configured anew)
    int gridShift_ = 0;            // a base cell is a level-gridShift_ tile
    int gridX0_ = 0, gridZ0_ = 0;  // the base cells' first (in base cells)
    int gridN_ = 0;                // base cells a side
    std::vector<Grid> grids_;      // a root's each
    std::vector<SurfaceItem> gridItems_;   // the items the grid holds (the bounds' whole ones when none)
    std::vector<Link> links_;
    int32_t freeLink_ = -1;
    uint32_t gridTouched_ = 0;     // base cells re-gathered by the last grid update

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
