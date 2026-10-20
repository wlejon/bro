#pragma once
// World cells (docs/scene-world-architecture.md §1): one hierarchy of cells,
// from the ground up, that content hangs off, so a frame's work follows the
// cells in view rather than everything in the world.
//
// Keys. A cell's key is integers, never floats: (root, level, x, z). A root
// is one quadtree's ground: a flat world is root 0 (a cube-sphere planet's
// face would be 0-5; nothing does sphere maths yet, but nothing assumes one
// plane either: every walk goes root by root). Level 0 is a leaf of
// CellGrid::leaf metres a side (64 by default), each level up twice the
// side, and a key's parent is its x and z shifted down (an arithmetic shift,
// so the grid has no seam at zero). Leaves are kept in quadtree order (by
// root, then top cell, then leaf), every leaf of a top cell
// (CellGrid::topLevel) together, so content sorted into that order is a
// contiguous run a leaf and a contiguous run a top.
//
// The provider contract. A content provider (the forest now; terrain,
// ground surface layers, grass and static meshes are meant to join)
// registers, for each leaf it has content in:
//   - the content's bounds, a box (its y range included), in the space the
//     views test it in;
//   - the run of its items there (in the provider's own cell order);
//   - the cell's origin, which its content may be stored relative to (by
//     default the leaf's corner on the ground, CellGrid::originOf). Each
//     frame the CPU takes every leaf's origin relative to the eye in double
//     (eyeRelativeOrigins) and the GPU cell pass hands it, in float, to each
//     cell in view: content stored relative to it keeps millimetres
//     anywhere in a world far past float's reach;
// and a generation, moved whenever any of that changes.
//
// What the GPU cell pass (scene_world_cells_pass.h) gives a provider each
// frame, for its cells in view of any view (the camera, each shadow tile):
// their list in cell order with their items' places, the views that see
// each, each one's eye-relative origin, its projected size in pixels in
// every view that sees it, and a band (near, mid, far) by the provider's own
// pixel thresholds on the camera's size. The size is the one pixel-error
// metric every system refines on: a cell's bounding radius over its distance
// times the view's focal length in pixels (a shadow tile: over its texel).
// A provider's geometric error in metres projects the same way:
// projectedPixels() below, the GLSL in world_cells_cull.comp.
//
// A cell also has room for streaming's residency state; nothing sets it yet.

#include <bromath/aabb.h>
#include <bromath/vec.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <functional>
#include <vector>

namespace bro::scene {

struct CellKey {
    int32_t x = 0, z = 0;
    uint8_t level = 0;   // 0 a leaf
    uint8_t root = 0;    // the quadtree (a flat world's 0)

    /// The cell `up` levels above (floored, so -1 >> 1 is -1: no seam at 0).
    CellKey parent(int up = 1) const { return {x >> up, z >> up, static_cast<uint8_t>(level + up), root}; }
    /// One integer: root in the top 3 bits, level in the next 5, then z and
    /// x in 28 bits each (two's complement): 2^27 leaves either side of 0.
    uint64_t packed() const {
        constexpr uint64_t kMask = (uint64_t{1} << 28) - 1;
        return uint64_t{root & 7u} << 61 | uint64_t{level & 31u} << 56 |
               (static_cast<uint64_t>(static_cast<uint32_t>(z)) & kMask) << 28 |
               (static_cast<uint64_t>(static_cast<uint32_t>(x)) & kMask);
    }
    static CellKey unpacked(uint64_t k) {
        const auto signed28 = [](uint64_t v) {
            return static_cast<int32_t>(static_cast<uint32_t>(v << 4)) >> 4;
        };
        return {signed28(k & ((uint64_t{1} << 28) - 1)), signed28((k >> 28) & ((uint64_t{1} << 28) - 1)),
                static_cast<uint8_t>((k >> 56) & 31u), static_cast<uint8_t>(k >> 61)};
    }
    bool operator==(const CellKey& o) const { return x == o.x && z == o.z && level == o.level && root == o.root; }
    bool operator!=(const CellKey& o) const { return !(*this == o); }
};

struct CellGrid {
    float leaf = 64.0f;   // a leaf's side, metres (in the provider's space)
    int topLevel = 3;     // the GPU cell pass's upper level: 8 x 8 leaves, 512 m

    /// The leaf of root `root` holding ground point (x, z).
    CellKey leafAt(float x, float z, uint8_t root = 0) const {
        return {static_cast<int32_t>(std::floor(x / leaf)), static_cast<int32_t>(std::floor(z / leaf)), 0, root};
    }
    CellKey topOf(const CellKey& k) const { return k.parent(topLevel - k.level); }
    /// Quadtree order: by root, then top cell (z, then x), then leaf.
    bool before(const CellKey& a, const CellKey& b) const;
    /// A cell's default origin: its corner on root 0's ground plane, in
    /// double (another root's is its own plane's until roots are a sphere's).
    void originOf(const CellKey& k, double out[3]) const {
        const double side = static_cast<double>(leaf) * static_cast<double>(int64_t{1} << k.level);
        out[0] = static_cast<double>(k.x) * side;
        out[1] = 0.0;
        out[2] = static_cast<double>(k.z) * side;
    }
};

/// The projected size in pixels of `metres` (a cell's bounding radius, a
/// provider's geometric error) at `distance` from the eye, in a view of
/// focal length `focalPixels` (pixels a metre at a metre); the cell pass's
/// size, so every system refines on one number.
inline float projectedPixels(float metres, float distance, float focalPixels) {
    return metres * focalPixels / std::max(distance, 1e-3f);
}

/// A cell's residency (streaming, §1): every cell is resident until a
/// provider streams.
enum class CellResidency : uint8_t { Resident };

/// One provider's content in one leaf: its bounds, its items' run
/// [first, first + count) in the provider's cell order, and its origin
/// (CellGrid::originOf's unless the provider sets one: `hasOrigin`).
struct CellContent {
    CellKey key;
    bromath::AABB3 bounds = bromath::aempty3();
    uint32_t first = 0, count = 0;
    double origin[3] = {0.0, 0.0, 0.0};
    bool hasOrigin = false;
};

/// Sort `count` items into cell order by their ground positions (`at(i)`:
/// item i's x, z, on root 0): `order[k]` is the k-th item in that order, a
/// leaf's items in their own order. Returns each leaf's run, in order, its
/// bounds empty (the provider sets them).
std::vector<CellContent> sortIntoCells(const CellGrid& grid, size_t count,
                                       const std::function<bromath::Vec2(size_t)>& at, std::vector<uint32_t>& order);

/// A cell of the tree, a leaf or a top: what its providers' content holds.
struct Cell {
    CellKey key;
    bromath::AABB3 bounds = bromath::aempty3();   // every provider's content in it
    uint64_t generation = 0;                      // the newest of its providers'
    uint32_t firstLeaf = 0, leaves = 0;           // a top: its leaves' run in leaves()
    double origin[3] = {0.0, 0.0, 0.0};           // CellGrid::originOf's
    CellResidency residency = CellResidency::Resident;   // (streaming: reserved)
};

class WorldCells {
public:
    using Provider = uint32_t;

    explicit WorldCells(const CellGrid& grid = {}) : grid_(grid) {}
    const CellGrid& grid() const { return grid_; }

    Provider addProvider();
    void removeProvider(Provider p);
    /// `p`'s content, replacing what it had: its leaves in cell order (as
    /// sortIntoCells gives them), each once. `generation` moves whenever
    /// the content does. A cell's origin left unset is the grid's.
    void setContent(Provider p, std::vector<CellContent> content, uint64_t generation);
    const std::vector<CellContent>& content(Provider p) const;
    uint64_t generation(Provider p) const;

    /// Every leaf any provider has content in, and the tops over them, in
    /// quadtree order (rebuilt when a provider's content is set).
    const std::vector<Cell>& leaves() const;
    const std::vector<Cell>& tops() const;

    /// Provider `p`'s cells as the GPU cell pass reads them
    /// (shaders/world_cells_layout.h): its tops, then its leaves, each with
    /// its bounds, run and key (a top never spans two roots).
    void flatten(Provider p, std::vector<uint32_t>& words) const;
    /// Provider `p`'s leaves' origins relative to `eye`, in double, then
    /// stored as floats (WC_ORIGIN_WORDS a leaf, in flatten's order).
    void eyeRelativeOrigins(Provider p, const double eye[3], float* out) const;

private:
    struct Entry {
        bool live = false;
        uint64_t generation = 0;
        std::vector<CellContent> content;
    };
    void rebuild() const;

    CellGrid grid_;
    std::vector<Entry> providers_;
    mutable std::vector<Cell> leaves_, tops_;
    mutable bool dirty_ = false;
};

}  // namespace bro::scene
