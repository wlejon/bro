#pragma once

#include <cmath>

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

    // Procedural detail below the data floor. The height layers stop at their
    // finest cell size; these octaves synthesise everything under it, so the
    // wavelength wants to start at roughly that floor rather than duplicating
    // structure the data already carries.
    float detailWavelength = 48.0f;   // coarsest synthesised octave, metres
    // Relief is a SLOPE, not a height: the amplitude of each octave is
    // detailRelief * (that octave's wavelength) * (the ground's own slope). So
    // it needs no retuning when a world changes heightScale, and it is 0 on
    // ground that is already flat. detailGain deviates from exact
    // self-similarity: 1 keeps every octave equally rough, below 1 smooths the
    // fine end, above 1 sharpens it.
    float detailRelief     = 0.35f;
    float detailGain       = 1.0f;
    int   detailOctaves    = 7;       // clamped to 8 by the shader

    float snowLine = 1600.0f;         // world metres, before per-place jitter
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

    /// Install a DETAIL EXEMPLAR: a patch of real terrain, in metres, whose
    /// structure is reused as the source of everything below the data floor.
    ///
    /// Gradient noise carries one feature per wavelength, so filling the
    /// decades between a coarse cell and the finest data costs an octave per
    /// decade and still produces rounded blobs — it has no notion of water
    /// running downhill, which is precisely what makes terrain read as terrain.
    /// A patch that came out of the decoder already has ridges, drainage and
    /// valley networks, and a mip chain over it is a band-limited multi-octave
    /// field for free: ONE tap at repeat length lambda delivers lambda down to
    /// lambda/width, with the fractional lod doing the anti-aliasing.
    ///
    /// The patch is high-passed (it supplies detail, not landforms), made
    /// exactly periodic, and divided by its own footprint at upload. That last
    /// step is what removes the magic number: relief-per-unit-length is
    /// dimensionless, so applying the patch at any wavelength reproduces the
    /// aspect ratio the model itself produced, at that scale.
    ///
    /// Pass data == nullptr to drop back to gradient noise.
    void setDetailExemplar(const float* data, int width, int height,
                           float metresPerCell);

    /// Per frame: park the node on the camera and push the camera uniforms.
    void update(float camX, float camY, float camZ);

    /// CPU sample of the SAME surface the GPU renders, in world metres —
    /// height layers plus procedural detail. This is what collision and camera
    /// grounding must use: detail displaces the drawn surface by metres, and a
    /// query that only knew about the layers would let everything fall through.
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

    // The detail lattice is anchored to the camera on this grid so its noise
    // coordinate stays small far from the world origin; see clipmap_detail.glsl
    // for why that matters and why the grid must divide every octave.
    //
    // It is therefore a whole number of coarsest-octave wavelengths, NOT a round
    // metre count: the shader turns the anchor into a lattice cell index with
    // ivec2(u_detailAnchor / lambda), so a fractional ratio truncates and leaves
    // a residue that CHANGES every time the anchor steps — the detail field
    // visibly jumps once per anchor cell as the camera travels. Every finer
    // octave is lambda/2^i, so a multiple of the coarsest divides them all.
    // ~256 m is the target; the wavelength decides the achievable value.
    //
    // Only octaves at or below detailWavelength are anchored — the band now
    // climbs above it wherever the data is coarse, and those octaves are wide
    // enough that absolute world coordinates keep full fp32 precision. See the
    // branch in cmDetail.
    float detailAnchorStep() const {
        const float lambda = cfg_.detailWavelength;
        if (!(lambda > 0.0f)) return 256.0f;
        const float n = std::round(256.0f / lambda);
        return lambda * (n < 1.0f ? 1.0f : n);
    }
    void recomputeHeightRange();

    /// The height-layer stack alone, without procedural detail.
    float baseElevationAt(float x, float z) const;

    /// Finest cell size the DATA resolves at this point, in metres — the top of
    /// the procedural band. Mirrors cmDataFloor() in clipmap_common.glsl.
    float dataFloorAt(float x, float z) const;

    /// High-passed, periodic, footprint-normalised exemplar. Empty until
    /// setDetailExemplar(); `exemplarN_` is its edge in texels.
    std::vector<float> exemplar_;
    int                exemplarN_ = 0;

    /// Bilinear tap into the exemplar with wraparound, matching the shader's
    /// GL_REPEAT sampling at lod 0. `u` is in repeats, not texels.
    float exemplarAt(float ux, float uz) const;

    /// Repeat length of the exemplar's coarse tap, in metres. Constant across
    /// the world by design — mirrors u_exLambda.
    float exemplarLambda() const;

    /// Octaves the detail band may climb ABOVE detailWavelength when the data
    /// underfoot is coarser than it. Must equal CM_DETAIL_UP_OCTAVES in
    /// clipmap_detail.glsl — the CPU query and the GPU surface diverge otherwise.
    static constexpr int kDetailUpOctaves = 8;

    /// Largest height the detail octaves can add, in metres. Feeds the cull
    /// margin, which has to bound what the shader will emit.
    float detailBound() const;

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
