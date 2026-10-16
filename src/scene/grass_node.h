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
// Drawn as three rings around the camera (fine blades near, coarser and
// wider ones further out, cross-fading through a dithered band), each one
// instanced draw over only the cells the view can see
// (vulkan/scene_mesh_drawer_grass.cpp).
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
        float width = 0.005f;       // blade width near the camera (m); each ring adds 0.2 x its cell
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
    };
    static constexpr int kParamCount = 17;
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

    /// The far blade (two segments) the outer rings draw.
    const bromesh::MeshData& farBlade() const { return farBlade_; }

    // --- Movers ---
    /// Track node `nodeId`: every frame its path through the grass stamps a
    /// bend of `radius` m (blades lean away and along its motion) and, with
    /// `trample` > 0, flattens it. radius <= 0 stops tracking.
    void setMover(uint32_t nodeId, float radius, float strength, float trample);
    size_t moverCount() const { return movers_.size(); }
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
    static constexpr float kBendRecovery = 2.2f;     // 1/s
    static constexpr float kTrampleRecovery = 0.05f; // 1/s

    // User-texture slot names (MeshNode::setCustomShaderTexture).
    static constexpr const char* kFieldTexture = "u_grassField";
    static constexpr const char* kPushTexture = "u_grassPush";

    // --- Rings ---
    static constexpr int kRings = 3;
    struct Ring {
        float cell = 0.08f;   // m
        float inner = 0.0f;   // m
        float outer = 26.0f;  // m
        float bandIn = 0.0f;  // cross-fade band at the inner edge
        float bandOut = 3.0f; // and at the outer edge
        float width = 0.02f;  // blade width (m)
    };
    /// The rings the current params give.
    Ring ring(int i) const;

    /// What the last frame drew, per ring: instances issued (the cells of
    /// the visible rectangle). The blades that survive culling are fewer.
    void setDrawnInstances(int ring, uint32_t n) { if (ring >= 0 && ring < kRings) drawn_[ring] = n; }
    uint32_t drawnInstances(int ring) const { return ring >= 0 && ring < kRings ? drawn_[ring] : 0; }

private:
    struct Mover {
        uint32_t nodeId = 0;
        float radius = 0.6f, strength = 1.0f, trample = 0.0f;
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
    bromesh::MeshData farBlade_;
    std::vector<float> field_;
    int fieldCols_ = 0, fieldRows_ = 0;
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
