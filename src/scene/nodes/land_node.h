#pragma once

// Land: the ground of a world, from underfoot to the horizon
// (docs/scene-world-architecture.md §13, "the land"). What the land is is
// the world's: a MODULE, GLSL compiled at run time
// (vulkan/shaders/ground/scene_land_layer.glsl says what it defines): its
// height at a point and its surface there (its albedo). The engine
// owns the rest:
//   - TILES of it, made by the world service (§6) as the view needs them
//     (vulkan/ground/scene_mesh_drawer_land.cpp): the module run over a
//     tile's texels in a compute pass, its heights and surface kept in a
//     surface layer set (world/surface_layers.h), chosen by their texels'
//     size on screen like every surface layer;
//   - the RINGS it is drawn with: concentric squares of fixed topology
//     parked on the camera, each level's cells twice the last's, so a
//     cell is about the same size on screen near and far; the vertex stage
//     (shaders/ground/mesh_land.vert) reads its height, normal and colour
//     from the tiles. Crack-free by construction: a vertex's height is a
//     function of its world position and its distance alone, so two rings
//     meet on the same surface (the derivation is mesh_land.vert's);
//   - the CPU's heights (heightAt, sampleHeights), for placement, physics
//     and the eye: the same FastNoise2 node tree the module's height was
//     emitted from (brokit's noise_glsl), run by FastNoise2 itself, so they
//     are exact and need no read-back.
//
// The land is square, `extent` metres from the origin each way; past it
// nothing is drawn. A LandNode is a MeshNode so the material setters
// (roughness, receivesShadow, visible) apply; its own mesh is the rings,
// its transform is ignored, and raycasts, heights and bounds read the land
// (raycast, sampleHeights, bounds), not the rings. It casts into the shadow
// pages (its hills' shadows, drawn with its ring blocks in each page's
// view) and receives them.

#include "scene/nodes/mesh_node.h"
#include "scene/world/fields.h"
#include "scene/world/surface_layers.h"

#include <cstdint>
#include <string>
#include <vector>

namespace bro::scene {

class LandNode : public MeshNode {
public:
    struct Rings {
        float cellSize = 0.5f;   // the finest ring's cell (m)
        int resolution = 256;    // cells a side a ring (a multiple of 4): a cell is
                                 // about (view height) * 2 / resolution pixels across
        int levels = 10;         // rings
    };

    explicit LandNode(const std::string& name = "");
    ~LandNode() override;

    LandNode* asLand() override { return this; }

    /// The world's land: `source` the module (scene_land_layer.glsl), `data`
    /// its landData(i) (at most kMaxData vec4s, as floats), `heightTree` the
    /// FastNoise2 encoded node tree its height is (the CPU's heights) and
    /// `seed` the seed it runs with. False with `err` when the tree does not
    /// decode. The module's GLSL is compiled at the first frame that draws
    /// it; one that does not compile is logged and draws nothing.
    bool setModule(std::string source, const float* data, size_t floats, const std::string& heightTree, int seed,
                   std::string& err);
    const std::string& moduleSource() const { return source_; }
    uint64_t moduleSourceHash() const { return sourceHash_; }
    const std::vector<float>& moduleData() const { return data_; }
    bool hasModule() const { return sourceHash_ != 0; }
    static constexpr size_t kMaxData = 64;   // vec4s

    /// The land's half-size (m): it covers [-extent, extent] in x and z.
    void setExtent(float extent);
    float extent() const { return extent_; }

    void setRings(const Rings& rings);
    const Rings& rings() const { return rings_; }

    /// The rings' mesh in blocks of kBlockCells x kBlockCells cells (fewer
    /// where a ring's hole cuts one), its indices block by block, so a pass
    /// draws the blocks its view reaches: the frame's camera behind it, a
    /// turf tile's fill its own 16 m of ground.
    static constexpr int kBlockCells = 32;
    struct Block {
        int ring = 0;
        int i0 = 0, j0 = 0, i1 = 0, j1 = 0;   // its cells, from the ring's corner
        uint32_t firstIndex = 0, indexCount = 0;
    };
    const std::vector<Block>& blocks() const { return blocks_; }
    /// Where block `b` lies while the rings are centred on `eye` (each
    /// ring's centre snapped to twice its cell, mesh_land.vert), over the
    /// land's height range.
    bromath::AABB3 blockBounds(const Block& b, const bromath::Vec3& eye) const;

    /// The height at world (x, z), metres: FastNoise2 on the module's tree
    /// (0 with none).
    float heightAt(float x, float z) const;
    /// Heights on a grid: (x0 + i * cell, z0 + j * cell), row by row; NaN
    /// off the land.
    void sampleHeights(float x0, float z0, float cell, int cols, int rows, float* out) const;
    /// Where a ray from `origin` along `dir` (unit) first meets the land
    /// within `maxDist`: its distance and the land's normal there. Marched
    /// over heightAt, then bisected to a centimetre.
    bool raycast(const bromath::Vec3& origin, const bromath::Vec3& dir, float maxDist, float& dist,
                 bromath::Vec3& normal) const;

    /// What the tiles are chosen over: the land's boxes (its height range in
    /// cells of its extent), and its generation (a new module or extent).
    const std::vector<SurfaceItem>& items() const { return items_; }
    const bromath::AABB3& bounds() const { return bounds_; }
    uint64_t generation() const { return generation_; }

    /// The tiles' layout (world/surface_layers.h): a level-0 tile's side is
    /// the finest ring's cell over 2 x its texels, so the finest ring reads
    /// two texels a cell.
    SurfaceLayerConfig tileConfig() const;

    /// The tiles' stats, the drawer's (scene.stats and land.stats()).
    struct Stats {
        uint32_t needed = 0, resident = 0, missing = 0, filled = 0, levels = 0;
        float shortfall = 1.0f;
        bool compiled = false;   // its module compiled (it draws)
    };
    void setStats(const Stats& s) { stats_ = s; }
    const Stats& stats() const { return stats_; }

private:
    void buildRings();
    void rebuildItems();

    std::string source_;
    uint64_t sourceHash_ = 0;
    std::vector<float> data_;
    FieldRef height_;
    float extent_ = 4096.0f;
    Rings rings_;
    std::vector<Block> blocks_;
    std::vector<SurfaceItem> items_;
    bromath::AABB3 bounds_ = bromath::aempty3();
    uint64_t generation_ = 1;
    Stats stats_;
};

}  // namespace bro::scene
