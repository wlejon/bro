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
// PRESENCE. A level is read only in a disc about the eye that its regions
// in cover whole: its cover is scaled by 1 - smoothstep(0.6 r, 0.9 r, d), d
// the distance along the ground from the level's centre (LandRegionCentre)
// and r its COVERED RADIUS there, the distance to the nearest ground of a
// wanted region not in, or to the wanted disc's edge, whichever is nearer
// (LandRegionLattice::covered). So a level fades into the coarser field in
// a circle, inside what is in, never along a region's straight edge (from
// the limb, level 3's regions still to come on the next cube face were a
// straight seam across the land); a level whose regions are streaming in
// has a smaller disc, growing as they arrive. LandRegionFade keeps a
// level's centre and radius: the centre follows the ground under the eye
// (moved on once the eye is a twentieth of the disc from it), and both are
// eased over some frames toward where they are going (the radius at once
// when it must shrink to stay inside what is in), so the disc never pops.
// When either changes, the land's items where the level's read changed are
// made again (LandNode::regionFadeChanged). The regions' own runs read the
// coarser field without it (procCoarse: what a region is made from must
// not depend on where the eye was).
//
// STREAMING. At every level whose cell is no smaller than a pixel from the
// eye's height (cell >= height / focal), the regions whose ground comes
// within `reach` lattice steps (half a step at reach 0) of the level's disc
// centre (the ground under the eye, LandRegionFade::target) are wanted: a
// disc, crossing a cube face's edges and corners onto the faces about it;
// coarser levels are made first (a region reads the coarser field it starts
// from), nearer regions first within a level. vulkan/ground/scene_land_regions.h
// runs them, a frame's budget of dispatches at a time.

#include "scene/world/place.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace bro::scene {

/// The uniform cubic B-spline's weights at fraction `f` between the middle
/// two of four cells: how the land's grids are read between their cell
/// centres, here and on the GPU (shaders/ground/land_grid.glsl's
/// landBspline). C2, so slopes and curvatures of a field read from cells
/// show no cell edges; it smooths, reading (a + 4 b + c) / 6 at a centre.
inline void landBspline(double f, double w[4]) {
    const double g = 1.0 - f, f2 = f * f, f3 = f2 * f;
    w[0] = g * g * g / 6.0;
    w[1] = (3.0 * f3 - 6.0 * f2 + 4.0) / 6.0;
    w[2] = (-3.0 * f3 + 3.0 * f2 + 3.0 * f + 1.0) / 6.0;
    w[3] = f3 / 6.0;
}

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

/// A point on the ground: a body direction (round, unit) or a chart point
/// (flat).
struct LandGroundPoint {
    double dir[3] = {0.0, 1.0, 0.0};
    double x = 0.0, z = 0.0;
    bool operator==(const LandGroundPoint& o) const {
        return dir[0] == o.dir[0] && dir[1] == o.dir[1] && dir[2] == o.dir[2] && x == o.x && z == o.z;
    }
};

/// Where a level is read about (PRESENCE above): its centre, and the
/// distances (chart metres flat; along the ground on a round body) over
/// which the level fades out (0.6 and 0.9 of its covered radius; both 0:
/// not read at all).
struct LandRegionCentre {
    bool active = false;   // none: the level is read wherever its regions are
    LandGroundPoint at;
    double inner = 0.0, outer = 0.0;
    bool operator==(const LandRegionCentre& o) const {
        return active == o.active && at == o.at && inner == o.inner && outer == o.outer;
    }
};

/// The regions in: an immutable snapshot, keyed by LandRegionKey::packed,
/// with each level's centre.
struct LandRegionSet : std::unordered_map<uint32_t, std::shared_ptr<const LandRegion>> {
    LandRegionCentre centre[LandRegionConfig::kMaxLevels];
};

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
    /// How much of a level is read at direction `dir` (round) or chart point
    /// (x, z) (flat) about its centre `c` (PRESENCE); 1 with none.
    double presence(const LandRegionCentre& c, const double dir[3], double x, double z) const;

    /// The distance between two points along the ground (chart metres flat).
    double between(const LandGroundPoint& a, const LandGroundPoint& b) const;
    /// The distance along the ground from `p` to the nearest ground region
    /// `k` has any weight on (0 inside it): never more than it is (on a round
    /// body its edges are sampled, less what the samples can miss).
    double distanceTo(const LandGroundPoint& p, const LandRegionKey& k) const;
    /// The wanted disc's radius at `level` (STREAMING).
    double discRadius(int level) const;
    /// Whether `level` is wanted from an eye `eyeHeight` over the ground with
    /// a focal length of `focalPixels` (its cell at least a pixel).
    bool levelWanted(int level, double eyeHeight, double focalPixels) const;
    /// The regions of `level` whose ground comes within `radius` of `p`, and
    /// their distances (distanceTo).
    void disc(int level, const LandGroundPoint& p, double radius,
              std::vector<std::pair<LandRegionKey, double>>& out) const;
    /// Level `level`'s covered radius about `centre` (PRESENCE): the distance
    /// to the nearest ground of a region of its wanted disc about `target`
    /// not in (`in` says which are), or to that disc's edge, whichever is
    /// nearer.
    template <class In>
    double covered(int level, const LandGroundPoint& centre, const LandGroundPoint& target, In in) const {
        std::vector<std::pair<LandRegionKey, double>> keys;
        const double radius = discRadius(level);
        disc(level, target, radius, keys);
        double r = radius - between(centre, target);
        for (const auto& [k, d] : keys)
            if (!in(k)) r = std::min(r, distanceTo(centre, k));
        return r > 0.0 ? r : 0.0;
    }

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
    void setCentres(const LandRegionCentre* centres);
    void clear();

private:
    mutable std::mutex mutex_;
    std::shared_ptr<const LandRegionSet> set_ = std::make_shared<const LandRegionSet>();
};

/// One level's PRESENCE as the eye moves and its regions come and go: its
/// disc's centre (`target`, the ground under the eye when the level was
/// last wanted), the centre it is read about (`at`, eased toward target)
/// and its radius (eased toward the covered radius about `at`).
struct LandRegionFade {
    /// The eye is this share of the disc's radius from target before target
    /// moves to it; each frame at and radius go this share of the way on
    /// (snapping to where they are going within kSnap of the disc).
    static constexpr double kFollow = 0.05;
    static constexpr double kEase = 0.3;
    static constexpr double kSnap = 0.005;
    static constexpr double kInner = 0.6, kOuter = 0.9;

    bool seen = false;
    LandGroundPoint target, at;
    double radius = 0.0;

    /// The level is wanted this frame, the eye over ground point `eye`.
    void aim(const LandRegionLattice& L, int level, const LandGroundPoint& eye);
    /// A frame on, `in` saying which of its regions are in: the centre the
    /// level is read about now (inactive until it has been aimed).
    template <class In>
    LandRegionCentre step(const LandRegionLattice& L, int level, In in) {
        if (!seen) return {};
        const double D = L.discRadius(level);
        ease(L, D);
        const double r = L.covered(level, at, target, in);
        radius += (r - radius) * kEase;
        if (std::fabs(r - radius) < kSnap * D) radius = r;
        // (The fade ends inside what is in: at once, when it must shrink.)
        radius = std::min(radius, r / kOuter);
        return centre();
    }
    LandRegionCentre centre() const;

private:
    void ease(const LandRegionLattice& L, double D);
};

/// Which regions are wanted about the eye: its body direction (unit) or
/// chart point, its height over the ground and the view's focal length in
/// pixels. In the order they should be made: coarse levels first, then the
/// nearest. Each level's disc is about `centres[level]` (null: the eye).
/// `near` gets the ones holding the ground under the eye.
void landRegionsWanted(const LandRegionLattice& lattice, const double eyeDir[3], double flatX, double flatZ,
                       double eyeHeight, double focalPixels, std::vector<LandRegionKey>& wanted,
                       std::vector<LandRegionKey>* near = nullptr, const LandGroundPoint* centres = nullptr);

/// Whether region `a`'s ground overlaps region `b`'s (same root; on a round
/// body regions of other faces are judged by their charts on `a`'s face).
bool landRegionsOverlap(const LandRegionLattice& lattice, const LandRegionKey& a, const LandRegionKey& b);

}  // namespace bro::scene
