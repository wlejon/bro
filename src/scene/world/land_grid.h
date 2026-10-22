#pragma once
// A land's PROCESSES and the grid they run on (docs/scene-world-architecture.md
// §11, "Processes"): GLSL compute kernels a world writes, iterated by the
// engine over a grid of the body, whose result the land's height and
// surface read (its first channel's x is the height the land adds to its
// tree). What runs them is vulkan/ground/scene_land_process.h; what a
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
// The grid is SAMPLED bilinearly between cell centres, the halo included,
// so a sample is continuous across a cube face's edge; the CPU (sample())
// and the GPU (shaders/ground/land_grid.glsl) take the same steps.

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
    /// root by root): bilinear between the cell centres either side, the
    /// halo's included, held at the halo's centres past them.
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

/// The height a land's processes add to its tree, as read back: shared,
/// never changed once made, by the land's CPU heights and the plants that
/// stand on it (world/plant_placement.h).
struct LandBase {
    LandGrid grid;
    std::vector<float> values;   // grid.texels()

    /// At a flat body's (x, z), at a round body's direction (unit).
    float at(double x, double z) const { return grid.sample(values.data(), 0, x, z); }
    float atDir(const double dir[3]) const { return grid.sampleDir(values.data(), dir); }
    float at(int root, double x, double z) const { return grid.sample(values.data(), root, x, z); }
};

}  // namespace bro::scene
