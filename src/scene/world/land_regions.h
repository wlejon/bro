#pragma once
// A land's REGIONAL GRIDS (docs/scene-world-architecture.md §11, "As built:
// regional grids"): finer grids of the land's processes, made under the eye
// as it moves and dropped behind it, each run with the world's own kernels
// from the next coarser field (the global grid's, or the coarser level's
// regions over it) and read where it is, so the shape a process makes (a
// valley a river cut, a ridge between basins) reaches down to a few tens of
// metres over the ground about the eye.
//
// LEVELS. Level 0's cells are `cell` chart metres; each level's are `ratio`
// times the last's. A level is a lattice of square REGIONS on each root of the
// land's chart (a round body's cube faces), each `cells` texels a side: a
// core of cells - 2 apron texels, the lattice's step, and an APRON of `apron`
// texels round it where it overlaps its neighbours. A region (level, root, i,
// j) has its texel t's centre at chart point -half + i step - apron cell +
// (t + 0.5) cell (and the same in z), where half is the root's half-size; on
// a round body a face's lattice runs on past the face's edges, so a region at
// an edge reaches over onto the next face (its chart goes on past the edge).
//
// BLENDING. A region weighs s(t) s(cells - t) across x and z, s the
// smoothstep over the apron's two widths, so the regions of a level sum to
// one wherever they are all present (a partition of unity: the overlaps are
// cross-fades, never seams). On a round body the faces are blended too: face
// f weighs 1 - smoothstep(half - b, half + b, max |chart|) (b one apron),
// normalised over the faces about the point, each face's regions read in its
// own chart. A level's value is the faces' sum of their regions' weighted
// values; where a level's regions cover less than all of a point (the level's
// edge, where regions are still to come), the coarser value makes up the
// rest. From the global grid down: value = coarser (1 - cover) + level's.
// The CPU (LandBase, land_grid.h) and the GPU (ground/land_regions.glsl) take
// the same steps.
//
// STREAMING. About the ground under the eye, at every level whose cell is
// no smaller than a pixel from the eye's height (cell >= height / focal),
// the regions within `reach` steps of the eye's lattice cell are wanted;
// coarser levels are made first (a region reads the coarser field it starts
// from), nearer regions first within a level. vulkan/ground/scene_land_regions.h
// runs them, a frame's budget of dispatches at a time.

#include "scene/world/place.h"

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace bro::scene {

struct LandRegionConfig {
    struct Pass {
        std::string fn;
        int iterations = 1;
    };
    static constexpr int kMaxLevels = 6;
    static constexpr int kMaxCells = 1024;

    std::string init;            // the regions' first values: void init(inout vec4 v[PROC_CHANNELS]) (none: no regions)
    std::vector<Pass> passes;    // run in order after init, as the global grid's are
    int cells = 512;             // texels a region's side
    int apron = 32;              // texels past its core each side (the cross-fade with its neighbours is two)
    double cell = 64.0;          // level 0's cell, chart metres
    int ratio = 4;               // each level's cell over the next finer's
    int levels = 3;
    int reach = 1;               // regions each way about the eye's, a level
    int keep = 2;                // channels kept and read (the first `keep`; the rest the global grid's)

    bool active() const { return !init.empty() && levels > 0; }
    int iterations() const;
    /// A level's cell, its lattice step and its face-blending band (chart metres).
    double cellOf(int level) const;
    double stepOf(int level) const { return (cells - 2 * apron) * cellOf(level); }
    double bandOf(int level) const { return apron * cellOf(level); }
    /// Clamped to the limits.
    void sanitize(int channels);
};

struct LandRegionKey {
    int level = 0, root = 0, i = 0, j = 0;
    /// One word (never 0): level 3 bits, root 3, i and j 11 each (+1024).
    uint32_t packed() const {
        return 1u + (static_cast<uint32_t>(level) | (static_cast<uint32_t>(root) << 3) |
                     (static_cast<uint32_t>(i + 1024) << 6) | (static_cast<uint32_t>(j + 1024) << 17));
    }
    bool operator==(const LandRegionKey& o) const {
        return level == o.level && root == o.root && i == o.i && j == o.j;
    }
};

/// A made region as the CPU reads it: its channel 0 (four floats a texel,
/// row by row).
struct LandRegion {
    LandRegionKey key;
    std::vector<float> texels;
};

/// The regions in: an immutable snapshot, keyed by LandRegionKey::packed.
using LandRegionSet = std::unordered_map<uint32_t, std::shared_ptr<const LandRegion>>;

/// A region's lattice, on a grid of `half` (the root's half-size) and body
/// radius `radius` (0 flat).
struct LandRegionLattice {
    LandRegionConfig config;
    double half = 0.0;
    double radius = 0.0;

    bool round() const { return radius > 0.0; }
    int roots() const { return round() ? 6 : 1; }
    /// Region `k`'s first texel's corner (chart metres).
    double originX(const LandRegionKey& k) const;
    double originZ(const LandRegionKey& k) const;
    /// The lattice indices a root's regions take at `level` (a face's run on
    /// past its edges by the band).
    void range(int level, int& lo, int& hi) const;
    /// The regions of `level` whose weight may be non-zero at chart coordinate
    /// `x` (one axis): [lo, hi].
    void candidates(int level, double x, int& lo, int& hi) const;
    /// A region's weight along one axis at chart coordinate `x` (origin `o`).
    double weight(int level, double o, double x) const;
    /// The faces about direction `dir` at `level` and their weights (summing
    /// to one): at most three; their charts' points in `chart`.
    int faces(int level, const double dir[3], int root[3], double w[3], double chart[3][2]) const;
    /// Channel 0 of the regions of `set` blended over `coarse` (four floats,
    /// the global grid's there) at body direction `dir` (round) or chart
    /// point (x, z) (flat): the blending above, level by level.
    void blendDir(const LandRegionSet& set, const double dir[3], float coarse[4]) const;
    void blendAt(const LandRegionSet& set, double x, double z, float coarse[4]) const;

    /// Regions held at most (every level's wanted ones, twice over for the
    /// faces about an edge), and the GPU's table's entries (a power of two,
    /// four times the slots: ground/land_regions.glsl's open addressing).
    int slots() const;
    int tableSize() const;
    /// The table's entry for `key`: where it is looked up first (the probe
    /// runs on from there), as the GLSL hashes it.
    static uint32_t hash(uint32_t packed, int tableSize);
    /// The GLSL that tells the shaders the regions' layout
    /// (ground/land_region_config.glsl's place); "" with none.
    std::string glslConfig() const;
};

/// What a land's regions are now, shared by its CPU heights and the plants
/// standing on it while the regions come and go (each change is a new
/// snapshot; a reader takes one and keeps it while it samples).
class LandRegionStore {
public:
    std::shared_ptr<const LandRegionSet> snapshot() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return set_;
    }
    void add(std::shared_ptr<const LandRegion> region);
    void remove(const LandRegionKey& key);
    void clear();

private:
    mutable std::mutex mutex_;
    std::shared_ptr<const LandRegionSet> set_ = std::make_shared<const LandRegionSet>();
};

/// Which regions are wanted about the eye: its body direction (unit), its
/// height over the ground and the view's focal length in pixels. In the order
/// they should be made: coarse levels first, then the nearest. `near` gets
/// the ones holding the ground under the eye.
void landRegionsWanted(const LandRegionLattice& lattice, const double eyeDir[3], double flatX, double flatZ,
                       double eyeHeight, double focalPixels, std::vector<LandRegionKey>& wanted,
                       std::vector<LandRegionKey>* near = nullptr);

/// Whether region `a`'s ground overlaps region `b`'s (same root; on a round
/// body regions of other faces are judged by their charts on `a`'s face).
bool landRegionsOverlap(const LandRegionLattice& lattice, const LandRegionKey& a, const LandRegionKey& b);

}  // namespace bro::scene
