#pragma once

// Grass: a meadow of procedural blades over a ground the node is given,
// bending in the scene's wind field and parted by movers. The GPU half is
// vulkan/shaders/scene_grass.glsl.
//
// No blade carries data. The node holds:
//   - the FIELD: a grid over the ground, four floats a texel (height,
//     density 0..1, dryness 0 lush .. 1 sun-dried, height multiplier),
//     set whole from a script (bro.scene's createGrass bakes it from a mesh,
//     a terrain, a clipmap or callbacks);
//   - the PARAMS: blade height and width, colours, range, slope limit and
//     the look's weights;
//   - the INFLUENCE MAP: a window of cells around the camera into which
//     movers (tracked nodes) and one-off pushes stamp a bend and a trample,
//     each cell keeping the time it was stamped so the shader decays it
//     (blades spring back in a fraction of a second, flattened grass rises
//     over tens of seconds) with no per-frame pass over the map.
//
// With a table of herb species and a floor giving their density, it also
// draws the low plants among the grass (setHerbs, scene_herb.glsl).
//
// Drawn as four rings around the camera, each one instanced draw over only
// the cells the view can see (vulkan/scene_mesh_drawer_grass.cpp). An
// instance is a cell's several blades (its ring mesh is that many copies of
// the blade end to end), since the GPU pays a fixed cost per instance: near,
// a tuft of fine blades fanning from one root; further out, fewer and wider
// blades scattered over a bigger cell, so blade count x width (the cover)
// stays about level across the rings while the vertex count falls. Rings
// cross-fade through a dithered band.
//
// A GrassNode is a MeshNode so the material setters (colour, roughness,
// subsurface, receivesShadow, castsShadow) apply; its own mesh is the blade
// and its transform is ignored (the blades stand on the field). It is left
// out of raycasts and bounds.

#include "scene/mesh_node.h"

#include <cstdint>
#include <vector>

namespace bro::scene {

class SceneGraph;

class GrassNode : public MeshNode {
public:
    explicit GrassNode(const std::string& name = "");
    ~GrassNode() override;

    GrassNode* asGrass() override { return this; }

    struct Params {
        float height = 0.4f;        // mean blade height (m)
        float width = 0.004f;       // blade width near the camera (m); the outer rings' are multiples of it
        float density = 1.0f;       // blades per area, x the default (cells shrink by sqrt)
        float range = 150.0f;       // the far edge of the outermost ring (m)
        float minNormalY = 0.62f;   // no grass where the ground is steeper than this
        float lush[3] = {0.12f, 0.17f, 0.03f};
        float dry[3] = {0.42f, 0.31f, 0.09f};
        float bulk = 0.72f;         // how much of a blade's shading follows the ground's normal
        float clumpFreq = 0.21f;    // clump noise frequency (1/m)
        float rootShade = 0.45f;    // the root's colour, x the blade's
        float sheen = 0.35f;        // a gust's lift of the bent blades
        float variety = 1.0f;       // large-scale height variation, 0 .. 1
        float wind = 1.0f;          // wind response, x
        // How the grass grows (scene_grass.glsl), each varying from place to
        // place about the value by shared noise and the field's dryness:
        float tufts = 0.35f;        // 0 a sward of many small tufts .. 1 bunch grass in tussocks with bare ground between
        float flowering = 0.08f;    // the share of tufts sending up a flowering culm (a seed head), more where lush and tall
        float dead = 0.08f;         // the share of a tuft's leaves that are dead (straw at its foot), more where dry
    };
    static constexpr int kParamCount = 20;
    void setParams(const Params& p);
    const Params& params() const { return params_; }
    /// The params as the flat list the bindings pass (setParams' order).
    void paramsToArray(float (&out)[kParamCount]) const;
    /// From the flat list; a NaN keeps the current value.
    void paramsFromArray(const double* v, size_t n);

    /// The field: `cols` x `rows` texels of 4 floats, texel (i, j) centred
    /// at world (x0 + i * cell, z0 + j * cell). Replaces the whole field.
    void setField(const float* rgba, int cols, int rows, float x0, float z0, float cell);
    bool hasField() const { return fieldCols_ > 1 && fieldRows_ > 1; }
    int fieldCols() const { return fieldCols_; }
    int fieldRows() const { return fieldRows_; }
    float fieldX0() const { return fieldX0_; }
    float fieldZ0() const { return fieldZ0_; }
    float fieldCell() const { return fieldCell_; }
    /// The field's ground height at world (x, z) (bilinear, as the GPU
    /// samples it); `fallback` outside it.
    float fieldHeight(float x, float z, float fallback = 0.0f) const;
    /// The ground's height range over world rect [x0, x1] x [z0, z1] and the
    /// largest height multiplier there, from coarse blocks of the field
    /// (conservative). False when nothing grows there (off the field, or
    /// density or height 0 throughout): the drawer skips the cells.
    bool fieldRange(float x0, float z0, float x1, float z1, float& lo, float& hi, float& tall) const;
    /// The floor on the field's grid, `channels` floats a texel (1 or 2):
    /// how closed the canopy overhead is (0 open .. 1 closed: litter on the
    /// turf, scene_ground.glsl, and the light the herbs choose their places
    /// by) and, with 2, how thickly herbs grow there (0 none .. 1 as their
    /// abundance has it; they grow there whether the grass does or not).
    /// Null clears it. A floor of another size than the field is ignored.
    void setFloor(const float* floor, int cols, int rows, int channels = 1);
    bool hasFloor() const { return !floor_.empty(); }

    // --- Herbs ---
    // The low plants among the grass (ferns, wildflowers, forbs, ground
    // cover), built where they stand from a table of species (broflora's
    // herbRecord, kHerbRecordFloats floats each): scene_herb_mix.glsl picks
    // one plant a cell of their own grid (the floor's canopy against each
    // species' shade, its colonies, the floor's herb density), scene_herb.glsl
    // builds it near the eye in two rings to `range`, then each plant as a
    // few of its largest cards, thinned and widened, to `far`; past that the
    // turf takes its colours.
    static constexpr int kHerbMax = 12;
    static constexpr int kHerbRecordFloats = 56;
    /// `count` species' records end to end (more than kHerbMax are dropped);
    /// the whole plants drawn to `range` m, their far form to `far` m (0
    /// keeps the current; a new range with no far puts it at 4 x the range),
    /// handing over to the turf through the last quarter of it.
    void setHerbs(const float* records, int count, float range = 0.0f, float far = 0.0f);
    int herbCount() const { return static_cast<int>(herbs_.size() / kHerbRecordFloats); }
    const std::vector<float>& herbRecords() const { return herbs_; }
    /// Herbs are drawn: species, and a floor with their density.
    bool hasHerbs() const { return herbCount() > 0 && herbFloor_; }
    /// The tallest species' height (m).
    float herbHeight() const { return herbHeight_; }
    /// What the turf takes from the herbs far off: their look averaged over
    /// the species' colonies (each in or out of one, by its patchCover), at
    /// a canopy of 0, 1/4 .. 1. kHerbMeanFloats a step: the cover exponent
    /// from above at full density (cover = 1 - exp(-density x it)), the
    /// leaves' mean rgb; the blooms' share of that cover, the blooms' mean
    /// rgb; the exponent seen from the side (a grazing view adds it over the
    /// tangent of its elevation), the blooms' share of that, the mean leaf
    /// top and bloom height (m).
    static constexpr int kHerbMeanSteps = 5;
    static constexpr int kHerbMeanFloats = 12;
    const float* herbMean() const { return herbMean_; }
    /// The herbs' grid: one plant at most a cell.
    static constexpr float kHerbCell = 0.35f;
    /// Near (every card), mid (fewer), far (a plant's few largest cards,
    /// thinned with distance and widened to keep their cover).
    static constexpr int kHerbRings = 3;
    struct HerbRing {
        float inner = 0.0f, outer = 10.0f, bandIn = 0.0f, bandOut = 1.5f;
        int cards = 14;      // cards an instance (a plant's blooms, stems, leaves)
        int segments = 5;    // a card's segments
    };
    HerbRing herbRing(int i) const;
    /// Where the whole plants end (the far ring begins).
    float herbRange() const { return herbRange_; }
    /// Where the far plants end, past which the turf stands in.
    float herbFar() const { return herbFar_; }
    /// Ring `i`'s mesh: `herbRing(i).cards` strips end to end.
    const bromesh::MeshData& herbRingMesh(int i) const { return herbMeshes_[i < 0 ? 0 : i >= kHerbRings ? kHerbRings - 1 : i]; }
    static int cardVertices(int segments) { return (segments + 1) * 2; }
    /// The ground's height range over a world rect where herbs grow (false:
    /// none there), from the field's blocks.
    bool herbRangeAt(float x0, float z0, float x1, float z1, float& lo, float& hi) const;
    /// The plants whose roots lie within `radius` of world (x, z), nearest
    /// first (at most `max`): where scene_herb.glsl stands them and which
    /// species each is (scene_herb_mix.glsl's herbPick, on the CPU).
    struct HerbPlant {
        float x = 0.0f, y = 0.0f, z = 0.0f;
        int species = 0;
    };
    std::vector<HerbPlant> herbPlants(float x, float z, float radius, size_t max) const;
    void setDrawnHerbs(int ring, uint32_t n) { if (ring >= 0 && ring < kHerbRings) herbDrawn_[ring] = n; }
    uint32_t drawnHerbs(int ring) const { return ring >= 0 && ring < kHerbRings ? herbDrawn_[ring] : 0; }


    // --- Movers ---
    /// Track node `nodeId`: every frame its path through the grass stamps a
    /// bend of `radius` m (blades lean away and along its motion) and, with
    /// `trample` > 0, flattens it. radius <= 0 stops tracking. It also
    /// pushes through shrubs and low foliage (plant_movers.h) up to
    /// `height` m above their roots.
    void setMover(uint32_t nodeId, float radius, float strength, float trample, float height = 1.8f);
    size_t moverCount() const { return movers_.size(); }
    struct MoverInfo { uint32_t nodeId; float radius, strength, height; };
    std::vector<MoverInfo> moverInfo() const;
    /// A one-off push this frame at world (x, z), moving at (vx, vz) m/s.
    void push(float x, float z, float radius, float vx, float vz, float strength, float trample);

    /// Stamp the movers and pushes into the influence map at wind-clock
    /// time `now`, recentring the map's window on the eye when it has
    /// wandered. Called once per frame by the renderer.
    void stamp(SceneGraph& graph, float now, float eyeX, float eyeZ);

    // The influence map's window: cell (0, 0)'s corner and the cell size.
    static constexpr int kPushCells = 512;
    static constexpr float kPushCell = 0.2f;
    float pushX0() const { return static_cast<float>(pushOx_) * kPushCell; }
    float pushZ0() const { return static_cast<float>(pushOz_) * kPushCell; }
    // A push's release (grassInfluence in scene_grass.glsl: keep in step).
    // Pushed blades spring back as a damped spring of kBendSpringHz and
    // ratio kBendSpringDamping: they swing back past upright once, a little,
    // and settle within about 1.5 s. A trampled lane rises again critically
    // damped (no sudden start): most of it over kTrampleRelax s, a faint
    // residue (kTrampleResidue of it) over kTrampleResidueRelax s. (A blade
    // shows a trample as a lean from its root, and a lean of a few hundredths
    // of a radian moves a fine blade's tip by its width: 6% over 4 s left a
    // third of the lane's change 8 s on, 0.6 s with 2% over 2.5 s a fifth.
    // At 0.35 s it is 90% risen by 1.4 s and gone by 3 s.)
    static constexpr float kBendSpringHz = 0.9f;
    static constexpr float kBendSpringDamping = 0.3f;
    static constexpr float kTrampleRelax = 0.35f;
    static constexpr float kTrampleResidue = 0.01f;
    static constexpr float kTrampleResidueRelax = 2.0f;
    /// The share of a stamped bend left `age` s after its stamp (1 at 0,
    /// level at 0, overshooting below 0 once).
    static float bendRelease(float age);
    /// The share of a stamped trample left `age` s after its stamp.
    static float trampleRelease(float age);

    // User-texture slot names (MeshNode::setCustomShaderTexture).
    static constexpr const char* kFieldTexture = "u_grassField";
    static constexpr const char* kPushTexture = "u_grassPush";
    static constexpr const char* kFloorTexture = "u_grassFloor";

    // --- Rings ---
    static constexpr int kRings = 4;
    struct Ring {
        float cell = 0.1f;     // m: one instance a cell
        float inner = 0.0f;    // m
        float outer = 7.0f;    // m
        float bandIn = 0.0f;   // cross-fade band at the inner edge
        float bandOut = 1.5f;  // and at the outer edge
        float width = 0.004f;  // blade width (m) at the eye; it grows with distance
        int blades = 14;       // blades an instance draws (copies in the ring mesh)
        int segments = 4;      // a blade's segments
        float spread = 0.035f; // how far an instance's blades stand from its root (m)
        float fan = 0.35f;     // how far a tuft's outer blades lean out (rad)
        // Thinning: past `thinFrom` m an instance keeps thinFrom / d of its
        // blades (never under `minKeep`), each widened by the inverse, so
        // the cover holds while the vertex work falls toward the next ring.
        float thinFrom = 3.5f;
        float minKeep = 0.5f;
    };
    /// The share of a ring's blades its shadow casters keep (widened by the
    /// inverse): a shadow map texel is wider than a fine blade.
    static constexpr float kShadowKeep = 0.35f;
    /// The rings the current params give.
    Ring ring(int i) const;
    /// Ring `i`'s mesh: `ring(i).blades` copies of its blade end to end.
    const bromesh::MeshData& ringMesh(int i) const;
    /// Vertices one blade of ring `i`'s mesh has.
    static int bladeVertices(int segments) { return segments * 2 + 1; }

    /// What the last frame drew, per ring: instances issued (the cells of
    /// the visible rectangle). The blades that survive culling are fewer.
    void setDrawnInstances(int ring, uint32_t n) { if (ring >= 0 && ring < kRings) drawn_[ring] = n; }
    uint32_t drawnInstances(int ring) const { return ring >= 0 && ring < kRings ? drawn_[ring] : 0; }

private:
    struct Mover {
        uint32_t nodeId = 0;
        float radius = 0.6f, strength = 1.0f, trample = 0.0f, height = 1.8f;
        bool seen = false;
        float lastX = 0.0f, lastZ = 0.0f;
        float vx = 0.0f, vz = 0.0f;
    };
    struct Push {
        float x, z, radius, vx, vz, strength, trample;
    };
    void stampDisc(float x, float z, float radius, float vx, float vz, float strength, float trample, float dt,
                   float now, int& minI, int& minJ, int& maxI, int& maxJ);
    void recentre(float eyeX, float eyeZ);
    void rebuildBlocks();
    void uploadField();

    Params params_;
    bromesh::MeshData ringMeshes_[kRings];
    std::vector<float> field_;
    std::vector<float> floor_;     // 2 a texel: canopy, herb density
    std::vector<float> fieldGpu_;  // the field as uploaded, the grass giving the herbs room (empty: field_ as is)
    bool herbFloor_ = false;       // the floor was given a herb density
    std::vector<float> herbs_;
    float herbHeight_ = 0.0f;
    float herbMean_[kHerbMeanSteps * kHerbMeanFloats] = {};
    void computeHerbMean();
    float herbRange_ = 30.0f;
    float herbFar_ = 90.0f;
    bromesh::MeshData herbMeshes_[kHerbRings];
    uint32_t herbDrawn_[kHerbRings] = {};
    int fieldCols_ = 0, fieldRows_ = 0;
    // Blocks of kFieldBlock x kFieldBlock texels: min height, max height,
    // the largest height multiplier where anything grows (0: nothing does)
    // and the largest herb density.
    static constexpr int kFieldBlock = 8;
    static constexpr int kBlockFloats = 4;
    // Field density under which nothing grows (setField).
    static constexpr float kDensityFloor = 0.06f;
    std::vector<float> blocks_;
    int blockCols_ = 0, blockRows_ = 0;
    float fieldX0_ = 0.0f, fieldZ0_ = 0.0f, fieldCell_ = 1.0f;

    std::vector<Mover> movers_;
    std::vector<Push> pushes_;
    std::vector<float> pushMap_;   // kPushCells^2 x (bend x, bend z, trample, stamp time)
    int pushOx_ = 0, pushOz_ = 0;  // the window's first cell (absolute)
    bool pushPlaced_ = false;
    bool pushUsed_ = false;        // anything ever stamped (an all-zero map needs no upload)
    float lastNow_ = -1.0f;
    uint32_t drawn_[kRings] = {};
};

}  // namespace bro::scene
