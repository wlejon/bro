#pragma once

// Grass: the cover over the land it stands on (land_node.h), a meadow of
// procedural blades bending in the scene's wind field and parted by movers.
// The GPU half is vulkan/shaders/ground/scene_grass.glsl.
//
// Where it grows is the world's land: its surface module says, at every
// point, how thickly the sward stands, how dry and how tall it is, the
// canopy over it and how thickly herbs grow (scene_land_layer.glsl's
// LandSurface::cover and herbs), read from the land's tiles as the view
// needs them, so the grass reaches as far as the land does. What the blades
// are is the world's too: a sward MODULE, GLSL compiled into the cover's
// shaders at run time (setSward, scene_sward_layer.glsl). No blade carries
// data. The node holds:
//   - its GROUND: the land node it stands on (setGround);
//   - the PARAMS: the near blade width, the rings' density and range;
//   - the INFLUENCE MAP: a window of cells around the camera into which
//     movers (tracked nodes) and one-off pushes stamp a bend and a trample,
//     each cell keeping the time it was stamped so the shader decays it
//     (blades spring back in a fraction of a second, flattened grass rises
//     over tens of seconds) with no per-frame pass over the map.
//
// With a world's herb module it also draws the low plants among the grass
// where the land grows them (setHerbs, scene_herb_layer.glsl).
//
// Drawn as four rings around the camera, each one instanced draw over only
// the cells the view can see (vulkan/ground/scene_mesh_drawer_grass.cpp). An
// instance is a cell's several blades (its ring mesh is that many copies of
// the blade end to end), since the GPU pays a fixed cost per instance: near,
// a tuft of fine blades fanning from one root; further out, fewer and wider
// blades scattered over a bigger cell, so blade count x width (the cover)
// stays about level across the rings while the vertex count falls. Rings
// cross-fade through a dithered band.
//
// A GrassNode is a MeshNode so the material setters (colour, roughness,
// subsurface, receivesShadow, castsShadow) apply; its own mesh is the blade
// and its transform is ignored (the blades stand on the land). It is left
// out of raycasts and bounds.

#include "scene/nodes/mesh_node.h"
#include "scene/world/place.h"

#include <cstdint>
#include <string>
#include <vector>

namespace bro::scene {

class SceneGraph;

class GrassNode : public MeshNode {
public:
    explicit GrassNode(const std::string& name = "");
    ~GrassNode() override;

    GrassNode* asGrass() override { return this; }

    struct Params {
        float width = 0.004f;       // blade width near the camera (m); the outer rings' are multiples of it
        float density = 1.0f;       // blades per area, x the default (cells shrink by sqrt)
        float range = 150.0f;       // the far edge of the outermost ring (m)
    };
    static constexpr int kParamCount = 3;
    void setParams(const Params& p);
    const Params& params() const { return params_; }
    /// The params as the flat list the bindings pass (setParams' order).
    void paramsToArray(float (&out)[kParamCount]) const;
    /// From the flat list; a NaN keeps the current value.
    void paramsFromArray(const double* v, size_t n);

    /// The land node the grass stands on (0: none, and nothing is drawn).
    void setGround(uint32_t landId);
    uint32_t ground() const { return ground_; }
    /// The most a land's cover multiplies a blade's height by
    /// (LandSurface::cover.z, land_fill.comp's clamp): the rings' culling
    /// boxes reach that high.
    static constexpr float kMaxTall = 2.0f;

    // --- Sward ---
    // The blades: a MODULE of the world's, GLSL compiled into the cover's
    // shaders at run time (shaders/ground/scene_sward_layer.glsl says what
    // it defines; vulkan/ground/scene_cover_shaders.h compiles it). It says
    // where a cell's blades rise and how thickly they stand, builds and
    // shades each blade at the width the rings ask for, and how the far
    // tier's tufts and the turf show the sward.
    static constexpr int kSwardDataVec4s = 64;   // scene_sward_layer.glsl's GRASS_SWARD_DATA
    /// The module's GLSL and its data (at most kSwardDataVec4s vec4s; read
    /// as swardData(i)); `height` its blades' mean height (m: where the far
    /// blades end, kFanEdgePixels, and their screen test), `tallest` the
    /// tallest a blade stands at a height multiplier of 1 and `reach` how far
    /// it moves a cell's root (m: the rings' culling boxes). An empty source:
    /// no blades.
    void setSward(std::string source, const float* data, size_t floats, float height, float tallest, float reach);
    const std::string& swardSource() const { return swardSource_; }
    /// A hash of swardSource() (0: none), which keys its compiled shaders.
    uint64_t swardSourceHash() const { return swardSourceHash_; }
    const std::vector<float>& swardData() const { return swardData_; }
    bool hasSward() const { return !swardSource_.empty(); }
    float swardHeight() const { return swardHeight_; }
    float swardTallest() const { return swardTallest_; }
    float swardReach() const { return swardReach_; }

    // --- Herbs ---
    // The low plants among the grass (ferns, wildflowers, forbs, ground
    // cover, or whatever a world grows there): a MODULE of the world's,
    // GLSL compiled into the cover's shaders at run time
    // (shaders/ground/scene_herb_layer.glsl says what it defines;
    // vulkan/ground/scene_cover_shaders.h compiles it). It says which plant
    // stands in each cell of the herbs' own grid, builds it near the eye in
    // two rings to `range`, then each plant as a few of its largest cards,
    // thinned and widened, to `far`, and how the far tier and the turf past
    // that show it.
    static constexpr int kHerbDataVec4s = 256;   // scene_herb_layer.glsl's GRASS_HERB_DATA
    /// The module's GLSL and its data (at most kHerbDataVec4s vec4s; read as
    /// herbData(i)); `height` the tallest its plants stand (m), which the
    /// rings' culling boxes take. The whole plants are drawn to `range` m,
    /// their far form to `far` m (0 keeps the current; a new range with no
    /// far puts it at 3 x the range), handing over to the turf through the
    /// last quarter of it. An empty source: no herbs.
    void setHerbs(std::string source, const float* data, size_t floats, float height, float range = 0.0f,
                  float far = 0.0f);
    const std::string& herbSource() const { return herbSource_; }
    /// A hash of herbSource() (0: none), which keys its compiled shaders.
    uint64_t herbSourceHash() const { return herbSourceHash_; }
    const std::vector<float>& herbData() const { return herbData_; }
    /// Herbs are drawn (where the land grows them): a module.
    bool hasHerbs() const { return !herbSource_.empty(); }
    /// The tallest the module's plants stand (m).
    float herbHeight() const { return herbHeight_; }
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
    /// The land's frame moved (a re-anchor on a round body): the influence
    /// map takes the move, so a trampled lane stays where it lies.
    void placeMoved(const PlaceMove& move);

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

    // User-texture slot name (MeshNode::setCustomShaderTexture).
    static constexpr const char* kPushTexture = "u_grassPush";

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

    Params params_;
    bromesh::MeshData ringMeshes_[kRings];
    uint32_t ground_ = 0;
    std::string swardSource_;
    uint64_t swardSourceHash_ = 0;
    std::vector<float> swardData_;
    float swardHeight_ = 0.4f;
    float swardTallest_ = 1.9f;
    float swardReach_ = 0.0f;
    std::string herbSource_;
    uint64_t herbSourceHash_ = 0;
    std::vector<float> herbData_;
    float herbHeight_ = 0.0f;
    float herbRange_ = 30.0f;
    float herbFar_ = 90.0f;
    bromesh::MeshData herbMeshes_[kHerbRings];
    uint32_t herbDrawn_[kHerbRings] = {};

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
