#pragma once

#include <memory>
#include <string>
#include <vector>

namespace bro::scene {

class SceneGraph;
class MeshNode;

// ---------------------------------------------------------------------------
// ClipmapTerrain — camera-centred geometry clipmap.
//
// A continuous world from underfoot to the horizon, with NO geometry rebuilds
// and NO chunk streaming. Concentric square rings of fixed topology are built
// once and parked on the camera; the GPU displaces them from a streamed height
// pyramid. Ring n covers 2x the area at the same triangle count, so
// screen-space triangle density is roughly constant from ground level to orbit.
//
// This is a SEPARATE facility from TerrainManager, which stays the
// voxel/editable terrain. Nothing here touches it.
//
// It is not a new draw path either: the whole thing is ONE MeshNode carrying
// static ring geometry plus a custom shader (src/scene/shaders/clipmap.*.glsl),
// so it inherits PBR lighting, fog, shadows, culling and the shadow-variant
// machinery from the existing mesh pipeline for free.
//
// Cracks are impossible by construction rather than by stitching — the vertex
// displacement is a pure function of world XZ evaluated at a FRACTIONAL mip
// level that depends only on distance from the camera. See the derivation
// comment in clipmap.vert.glsl.
// ---------------------------------------------------------------------------

struct ClipmapConfig {
    int   levels      = 10;     // concentric rings
    int   resolution  = 128;    // quads per level per axis (rounded to a
                                // multiple of 4 — the central hole is
                                // resolution/2 quads inset by resolution/4)
    float cellSize    = 1.0f;   // metres per cell at level 0
    float heightScale = 1.0f;   // sampled value -> metres
    float seaLevel    = 0.0f;   // metres added to every sample
};

/// One level of the height pyramid: an R32F mipmapped texture plus where it
/// sits in the world. Layers are ordered FINEST FIRST; the coarsest present
/// layer is assumed to cover everything and is the base of the blend.
struct ClipmapLayer {
    std::vector<float> data;    // width*height row-major samples (CPU mirror)
    int   width  = 0;
    int   height = 0;
    float originX = 0.0f;       // world metres of texel 0
    float originZ = 0.0f;
    float metresPerCell = 1.0f;
    bool  present = false;
};

class ClipmapTerrain {
public:
    static constexpr int kMaxLayers = 4;

    /// Builds the ring mesh and installs the node + custom shader immediately.
    ClipmapTerrain(SceneGraph& graph, const ClipmapConfig& cfg);
    ~ClipmapTerrain();

    ClipmapTerrain(const ClipmapTerrain&) = delete;
    ClipmapTerrain& operator=(const ClipmapTerrain&) = delete;

    const ClipmapConfig& config() const { return cfg_; }

    /// Install (or, with data == nullptr, release) height layer `index`.
    /// Layers are finest-first and expected to be contiguous from 0; an absent
    /// slot in the middle simply contributes zero weight. A released slot keeps
    /// a 1x1 placeholder texture bound so the sampler is never unbound, but
    /// drops the pixel data.
    void setHeightLayer(int index, const float* data, int width, int height,
                        float originX, float originZ, float metresPerCell);

    /// Per frame: park the node on the camera and push the camera uniforms.
    void update(float camX, float camY, float camZ);

    /// CPU sample of the SAME layer stack the GPU renders, in world metres.
    /// Same layer selection and coverage blending; bilinear at mip level 0
    /// within each layer. Exact near the camera (where the GPU also samples
    /// level 0) and approximate far away, where the GPU has moved to a coarser
    /// mip — see docs/clipmap-api.js.
    float elevationAt(float x, float z) const;

    MeshNode* node() const { return node_; }

    int levelCount() const { return cfg_.levels; }
    int triangleCount() const { return triangleCount_; }
    int vertexCount() const { return vertexCount_; }
    int layerCount() const { return layerCount_; }

    /// Destroy the node and drop every layer. Idempotent.
    void destroy();

private:
    void buildGeometry();
    void pushLayerUniforms();
    void pushStaticUniforms();
    void recomputeHeightRange();

    SceneGraph&   graph_;
    ClipmapConfig cfg_;
    MeshNode*     node_ = nullptr;

    ClipmapLayer layers_[kMaxLayers];
    int layerCount_ = 0;          // highest present index + 1

    // Sample extremes across every present layer, in world metres. Drives the
    // per-update cull margin: frustum culling cannot see GLSL displacement.
    float minHeight_ = 0.0f;
    float maxHeight_ = 0.0f;

    int triangleCount_ = 0;
    int vertexCount_   = 0;
};

} // namespace bro::scene
