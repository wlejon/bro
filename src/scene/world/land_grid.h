#pragma once
// A land's PROCESSES and the grid they run on (docs/scene-world-architecture.md
// §11, "Processes"): GLSL compute kernels a world writes, iterated by the
// engine over a grid of the body, whose result the land's height and
// surface read (its first channel is the land's height over its tree: x
// added, y the tree's scale). What runs them is vulkan/ground/scene_land_process.h; what a
// kernel may call is shaders/ground/land_process_layer.glsl.
//
// The GRID is the land's chart in cells: a flat body's one root, a square
// of `resolution` cells a side over [-extent, extent]; a round body's six
// cube faces (place.h's chart), each `resolution` cells a side. Each root
// is stored with a halo of one cell round it, so a kernel reads its eight
// neighbours everywhere: on a flat body the halo repeats the edge's cells;
// on a round one it holds the cells of the neighbouring faces nearest each
// halo cell's centre, copied from them after every pass (halo()), and at
// each corner the mean of the three faces' corner cells.
//
// The grid is SAMPLED by the cubic B-spline over the cell centres about a
// point (landBspline, land_regions.h), the halo included, so a sample and
// its slopes and curvatures are continuous across the cells' edges (across
// a cube face's to within the halo's one cell); the CPU (sample()) and the
// GPU (shaders/ground/land_grid.glsl) take the same steps.

#include "scene/world/land_regions.h"
#include "scene/world/place.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace bro::scene {

/// What a world asks to be run: its kernels and the grid they run on.
struct LandProcess {
    struct Pass {
        std::string fn;       // the kernel: void fn(inout vec4 v[PROC_CHANNELS])
        int iterations = 1;
    };
    static constexpr int kMaxChannels = 4;
    static constexpr int kMaxResolution = 2048;
    static constexpr size_t kMaxData = 64;   // vec4s

    std::string source;          // the process module (land_process_layer.glsl): procInit and the passes
    std::vector<Pass> passes;    // run in order after procInit
    int resolution = 256;        // cells a root side
    int channels = 1;            // vec4s a cell
    std::vector<float> data;     // procData(i), vec4s as floats
    LandRegionConfig regions;    // the regional grids under the eye (land_regions.h; none: init empty)

    bool active() const { return !source.empty(); }
    /// Every pass's iterations.
    int iterations() const;
};

class LandGrid {
public:
    /// One halo cell's copy: texel indices (root * side + y) * side + x,
    /// the mean of three sources (the same one three times but at a cube's
    /// corner, where three faces' corner cells meet and the halo's corner
    /// is their mean).
    struct HaloCopy {
        uint32_t dst = 0, src[3] = {0, 0, 0};
    };

    /// The grid of `resolution` cells a root side: over [-extent, extent]
    /// on a flat body, the six cube faces of a round one.
    void configure(const Body& body, double extent, int resolution);
    bool configured() const { return cells_ > 0; }

    const Body& body() const { return body_; }
    bool round() const { return radius_ > 0.0; }
    int roots() const { return round() ? 6 : 1; }
    /// Cells a root side, and with its halo.
    int cells() const { return cells_; }
    int side() const { return cells_ + 2; }
    /// A root's half-size and a cell's side, in chart metres.
    double half() const { return half_; }
    double cell() const { return 2.0 * half_ / cells_; }
    double radius() const { return radius_; }
    /// Texels a root (side()²) and in all.
    size_t rootTexels() const { return static_cast<size_t>(side()) * side(); }
    size_t texels() const { return rootTexels() * roots(); }

    const std::vector<HaloCopy>& halo() const { return halo_; }

    /// Chart point (x, z) of `root` sampled from `values` (texels() floats,
    /// root by root): the cubic B-spline over the four cell centres about
    /// it each way, the halo's included, held at the halo's past them.
    float sample(const float* values, int root, double x, double z) const;
    /// At a body direction (unit; a round body's), on the face it lies on.
    float sampleDir(const float* values, const double dir[3]) const;

    /// The GLSL that tells the shaders the grid's layout
    /// (shaders/ground/land_grid_config.glsl's place), for `channels`.
    std::string glslConfig(int channels) const;

private:
    void buildHalo();

    Body body_;
    double radius_ = 0.0;
    double half_ = 0.0;
    int cells_ = 0;
    std::vector<HaloCopy> halo_;
};

/// A land's processes' height over its tree's, as read back (their channel
/// 0: x added, y the tree's scale), and the world's own two values beside
/// it (z and w, what its plants' rules may read: world/plant_placement.h's
/// PlacementTerm), with the regional grids' channel 0 blended over it where
/// they are in (land_regions.h): shared by the land's CPU heights and the
/// plants that stand on it. The global grid's values never change once made;
/// the regions come and go (`regions`' snapshots).
struct LandBase {
    LandGrid grid;
    std::vector<float> values;   // the height added, grid.texels()
    std::vector<float> scales;   // the tree's scale, grid.texels() (none: 1)
    std::vector<float> own[2];   // channel 0's z and w, grid.texels() each (none: 0)
    LandRegionLattice lattice;                        // the regions' (config.active() false: none)
    std::shared_ptr<const LandRegionStore> regions;   // the regions in (null: none)

    /// Channel 0 (x the height added, y the tree's scale, z and w the world's
    /// own) at a root's chart point, at a round body's direction (unit): the
    /// global grid's, the regions' blended over it.
    void field(int root, double x, double z, float out[4]) const;
    void fieldDir(const double dir[3], float out[4]) const;

    /// The world's own value k (0 z, 1 w) at a root's chart point, at a
    /// round body's direction (unit).
    float ownAt(int k, int root, double x, double z) const {
        float v[4];
        field(root, x, z, v);
        return v[2 + k];
    }
    float ownDir(int k, const double dir[3]) const {
        float v[4];
        fieldDir(dir, v);
        return v[2 + k];
    }

    /// The land's height over the tree's `tree` at a flat body's (x, z),
    /// at a round body's root and chart point, at its direction (unit).
    float over(float tree, double x, double z) const { return over(tree, 0, x, z); }
    float over(float tree, int root, double x, double z) const {
        float v[4];
        field(root, x, z, v);
        return v[0] + v[1] * tree;
    }
    float overDir(float tree, const double dir[3]) const {
        float v[4];
        fieldDir(dir, v);
        return v[0] + v[1] * tree;
    }

private:
    void global(int root, double x, double z, float out[4]) const;
};

}  // namespace bro::scene
