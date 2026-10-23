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
//     (shaders/ground/mesh_land.vert) reads its height from the tiles, and
//     the fragments (draw/mesh.frag's SCENE_LAND build) its albedo and
//     normal at their own footprint, so the ground's detail is its tiles'
//     texels, not the rings' cells. Crack-free by construction: a vertex's height is a
//     function of its world position and its distance alone, so two rings
//     meet on the same surface (the derivation is mesh_land.vert's);
//   - the CPU's heights (heightAt, sampleHeights), for placement, physics
//     and the eye: the same FastNoise2 node tree the module's height was
//     emitted from (brokit's noise_glsl), run by FastNoise2 itself, so they
//     are exact, plus its PROCESSES' height (world/land_grid.h): kernels
//     run over a grid of the land once when it is made, read back once and
//     sampled as the GPU samples it.
//
// On a flat body the land is square, `extent` metres from the origin each
// way; past it nothing is drawn. On a round body (world/place.h) it is the
// whole body (docs/scene-world-architecture.md §11, "The land on the
// body"): its tiles are keyed on the cube faces of the body's chart, its
// rings laid out in the land's map about the anchor, its height a 3D field
// at the body's points, and its CPU calls take the land's frame (the place
// frame there; SceneView::world everywhere). A LandNode is a MeshNode so the material setters
// (roughness, receivesShadow, visible) apply; its own mesh is the rings,
// its transform is ignored, and raycasts, heights and bounds read the land
// (raycast, sampleHeights, bounds), not the rings. It casts into the shadow
// pages (its hills' shadows, drawn with its ring blocks in each page's
// view) and receives them.

#include "scene/nodes/mesh_node.h"
#include "scene/world/body_surface_map.h"
#include "scene/world/fields.h"
#include "scene/world/land_grid.h"
#include "scene/world/place.h"
#include "scene/world/surface_layers.h"

#include <algorithm>
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
        int levels = 10;         // rings (on a round body at least as many as reach round it)
    };
    static constexpr int kMaxRings = 20;

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

    /// The land's processes (world/land_grid.h; none: `process.source`
    /// empty): kernels run over its grid when it is made, whose first
    /// channel's x the land's height adds to its tree. False with `err`
    /// when a pass names no function. A land with processes is not ready
    /// (it draws nothing and its heights are NaN) until they have run and
    /// been read back (vulkan/ground/scene_land_process.h).
    bool setProcess(LandProcess process, std::string& err);
    const LandProcess& process() const { return process_; }
    const LandGrid& grid() const { return grid_; }
    /// Moved whenever the grid must be run again (its processes, extent or body).
    uint64_t gridSerial() const { return gridSerial_; }
    /// The grid's layout for the shaders ("" with no processes), and the
    /// switch over its passes (land_process_dispatch.glsl's place).
    std::string gridConfig() const;
    std::string processDispatch() const;
    /// The processes' first channel (x the height added, y the tree's scale,
    /// z and w the world's own: LandBase), 4 floats every texel of the grid,
    /// read back for grid serial `serial`.
    void setGridBase(uint64_t serial, std::vector<float> channel0);
    /// It has no processes, or theirs is in.
    bool ready() const { return !process_.active() || baseSerial_ == gridSerial_; }
    /// What its processes add to its tree (null with none, or until ready):
    /// what the plants standing on it add to their ground.
    std::shared_ptr<const LandBase> groundBase() const { return base_; }

    /// The canopy of the plants standing on the land (world/plant_canopy.h's
    /// module, "" none), the tallest they stand (m) and how far from the eye
    /// they are drawn and cast: kept in the tiles beside the ground, drawn
    /// as the land past the plants' range, and shading the land's sun past
    /// their cast range (docs/scene-world-architecture.md §5, "the canopy
    /// layer" and "the land's sun").
    void setCanopy(std::string source, float top, float range, float castRange);
    const std::string& canopySource() const { return canopy_; }
    uint64_t canopySourceHash() const { return canopyHash_; }
    float canopyTop() const { return canopyTop_; }
    float canopyRange() const { return canopyRange_; }
    float canopyCastRange() const { return canopyCastRange_; }

    /// Its sea: water filling the land up to `level` (m over the datum)
    /// wherever its ground lies lower, decided a fragment at a time, so a
    /// coast is as sharp as the pixels are whatever its tiles' texels.
    /// Light crossing it loses `absorb` (1/m, each channel) a metre, the
    /// seabed showing through shallow water, and deep water is `scatter`
    /// (the albedo the light it sends back gives it); its surface is as
    /// rough as `roughness`. The ground's heights (landHeight, placement)
    /// stay the seabed's.
    struct Sea {
        bool on = false;
        float level = 0.0f;
        float absorb[3] = {0.45f, 0.09f, 0.05f};
        float scatter[3] = {0.004f, 0.016f, 0.03f};
        float roughness = 0.08f;
    };
    void setSea(const Sea& sea) { sea_ = sea; }
    const Sea& sea() const { return sea_; }

    /// The land's half-size (m): on a flat body it covers [-extent, extent]
    /// in x and z. (A round body's land is the whole body.)
    void setExtent(float extent);
    float extent() const { return extent_; }

    /// The body it lies on, its graph's (SceneGraph keeps it set; null or a
    /// flat body: the plane). A new body is a new land (its generation).
    void setPlace(const Place* place);
    const Place* place() const { return place_; }
    /// On a round body.
    bool round() const { return place_ && !place_->body().flat(); }
    /// What the tiles are keyed on and how far the map reaches: the chart's
    /// map (null on the plane) and the map's half-size (the extent; on a
    /// round body to the antipode, πR).
    const SurfaceMap* tileMap() const { return round() ? &map_ : nullptr; }
    double mapReach() const;

    void setRings(const Rings& rings);
    const Rings& rings() const { return rings_; }
    /// The rings drawn: rings().levels, on a round body more if it takes
    /// more to reach round it.
    int ringLevels() const { return ringLevels_; }

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
    /// Where block `b` lies (the land's frame) while the rings are centred
    /// on `eye` (each ring's centre snapped to twice its cell on the map,
    /// mesh_land.vert), over the land's height range, ring 0 drawn as ring
    /// `first` (firstRing); `hidden` set when the body's curve hides all of
    /// it from the eye.
    bromath::AABB3 blockBounds(const Block& b, const bromath::Vec3& eye, int first = 0,
                               bool* hidden = nullptr) const;
    /// The finest ring drawn from `eye` (a view of `focalPixels`): the rings
    /// whose cells are under a pixel of ground from its height are not, as
    /// a geometry clipmap drops its fine levels as it climbs, and ring 0's
    /// full grid is drawn as that ring (its blocks' ring the ones between
    /// are left out).
    int firstRing(const bromath::Vec3& eye, float focalPixels) const;
    /// How high land's-frame `eye` stands over the ground under it (on a
    /// round body along its up).
    float eyeAbove(const bromath::Vec3& eye) const;
    /// Whether the block `b` is drawn when ring 0 is drawn as `first`.
    static bool blockDrawn(const Block& b, int first) { return b.ring == 0 || b.ring > first; }

    /// The ground's height at land's-frame (x, z), metres: FastNoise2 on the
    /// module's tree (0 with none) plus its processes'; on a round body the
    /// y of the ground under (x, z) in the place frame (its curve
    /// included). NaN until the land is ready().
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
    /// cells of its chart: of its extent, or of each cube face), every
    /// root's chart bounds with its height range, and its generation (a new
    /// module, extent or body).
    const std::vector<SurfaceItem>& items() const { return items_; }
    const bromath::AABB3& chartBounds() const { return chartBounds_; }
    /// The land's box in its frame (on a round body the body's, about the
    /// place frame as it is now).
    bromath::AABB3 bounds() const;
    /// Its lowest and highest ground.
    float lowest() const { return chartBounds_.min.y; }
    float highest() const { return sea_.on ? std::max(chartBounds_.max.y, sea_.level) : chartBounds_.max.y; }
    /// The land's height range over world rect [x0, x1] x [z0, z1], from its
    /// items (conservative); false off the land.
    bool heightRange(float x0, float z0, float x1, float z1, float& lo, float& hi) const;
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
    int wantedRings() const;
    void rebuildItems();
    void rebuildFaceItems();
    /// A round body's ground height (over the datum) at body direction `dir`.
    float heightOver(const double dir[3]) const;
    /// The height over the tree's `tree` at chart point (x, z) of `root`
    /// (the tree's with no processes).
    float overTree(float tree, int root, double x, double z) const;
    void configureGrid();

    const Place* place_ = nullptr;
    double radius_ = 0.0;   // the body's the items were made for (0 the plane)
    BodySurfaceMap map_;
    std::string source_;
    uint64_t sourceHash_ = 0;
    std::vector<float> data_;
    std::string canopy_;
    uint64_t canopyHash_ = 0;
    float canopyTop_ = 0.0f;
    float canopyRange_ = 0.0f, canopyCastRange_ = 0.0f;
    FieldRef height_;
    LandProcess process_;
    LandGrid grid_;
    uint64_t gridSerial_ = 1, baseSerial_ = 0;
    std::shared_ptr<const LandBase> base_;
    float extent_ = 4096.0f;
    Rings rings_;
    Sea sea_;
    int ringLevels_ = 0, builtResolution_ = 0;   // the mesh's
    std::vector<Block> blocks_;
    std::vector<SurfaceItem> items_;
    int itemCells_ = 0;   // items_ is itemCells_ x itemCells_ a root, row by row from the chart's corner
    bromath::AABB3 chartBounds_ = bromath::aempty3();
    uint64_t generation_ = 1;
    Stats stats_;
};

}  // namespace bro::scene
