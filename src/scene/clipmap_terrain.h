#pragma once

#include <algorithm>
#include <cmath>
#include <limits>

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

    // Largest power-of-two zoom of the ring stack (see ClipmapTerrain::update).
    // 1 pins the stack to cellSize and gives it a fixed, finite footprint —
    // the world then simply ENDS at farDistance, which from altitude is a lip
    // of terrain hanging in the sky. The cap exists because the app has to
    // supply height data across whatever footprint the stack claims, and
    // extent grows linearly with this.
    float maxCellScale = 64.0f;

    // Planet radius in metres; 0 draws a flat world. Curvature is what bounds
    // the terrain — see cmCurve in clipmap_common.glsl and horizonDistance().
    // Earth is 6371000.
    float planetRadius = 0.0f;
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
    // Periodic in X. Set for a global equirectangular chart, where column 0
    // continues column W-1 and there is no east-west edge to fade out at.
    bool  wrapX   = false;
    // The samples carry real content down to twice metresPerCell, so procedural
    // detail must own everything finer and nothing coarser. Off by default,
    // which leaves the cell-size heuristic in clipmap_detail.glsl to guess —
    // see setHeightLayer().
    bool  bandLimited = false;
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
    /// `wrapX` marks the layer periodic in X — a global equirectangular chart,
    /// where column 0 continues column W-1. Such a layer is sampled GL_REPEAT
    /// in S (still clamped in T, since latitude does not wrap) and its coverage
    /// ramp has no east-west edge to fade at.
    ///
    /// `bandLimited` marks the layer as carrying real content all the way down
    /// to twice metresPerCell, so procedural detail high-passes against that and
    /// adds nothing coarser. Leave it OFF for a smooth learned/decoded field:
    /// such a layer has a fine cell but no sub-kilometre ruggedness in it, and
    /// the detail band then roughens it from a fixed ceiling instead (see
    /// CM_ROUGHEN_* in clipmap_detail.glsl), which is what keeps a decoded
    /// mountainside from reading as glass. Turn it ON for a field that was
    /// generated or eroded at its own cell size: roughening one of those lays
    /// octaves on top of a band the data already fills, and every extra live
    /// octave adds another detailRelief to the shading tangent until the normal
    /// tips past the terminator and steep ground speckles.
    void setHeightLayer(int index, const float* data, int width, int height,
                        float originX, float originZ, float metresPerCell,
                        bool wrapX = false, bool bandLimited = false);

    void setSnowLine(float snowLine);
    void setDetail(float wavelength, float relief, float gain, int octaves);
    void setMaterials(const float* rockAlbedo, float rockRoughness,
                      const float* snowAlbedo, float snowRoughness,
                      const float* sandAlbedo, float sandRoughness,
                      const float* grassAlbedo, float grassRoughness);
    /// L0 forest canopy tint: recolour forest-biome ground toward `albedo`
    /// (linear rgb) at `strength` in [0,1]. strength 0 disables it.
    void setForest(const float* albedo, float strength);
    /// Install (or, with data == nullptr, release) surface layer `index`:
    /// `components` control channels per texel (3 or 4), resampled and blended
    /// by exactly the same coverage rule the height layers use. Finest-first
    /// and contiguous from 0, like setHeightLayer.
    ///
    /// FOUR CHANNELS, AND WHY THE THIRD IS NOT ENOUGH. Three was never a
    /// property of anything — it was the width of the first caller. A layer
    /// carrying drainage, sediment and hardness has no room left for the one
    /// channel that means the same thing at 32 m and at 2 km: climate. Storage
    /// is always RGBA internally, and a caller supplying three components gets
    /// w = 0 rather than GL's fill of 1 — a channel nobody supplied should read
    /// as absent, not as saturated, and 1.0 is a value some shader will treat
    /// as "maximally wet" on the day it is added.
    ///
    /// WHY THIS IS INDEXED. It used to store one struct and take no index,
    /// which quietly made control channels a property of the FINEST layer
    /// alone. That is fine for a single-chart world and wrong for a stack: a
    /// clipmap reaching 262 km with a 33 km fine layer has one layer's drainage
    /// and lithology painted, clamped, across an entire aerial frame — or faded
    /// to neutral, which is the same information loss wearing a nicer coat. A
    /// height pyramid whose control channels stop at layer 0 describes the
    /// shape of distant ground and nothing about what it is made of.
    ///
    /// THE ONE CAVEAT, and it is a real one: the blend is LINEAR PER CHANNEL,
    /// because that is what the height blend is and two different rules across
    /// the same seam is worse than one imperfect rule. A channel carrying a
    /// QUANTITY (moisture, drainage, hardness, temperature) blends correctly. A
    /// channel carrying an ID does not — halfway across a fade between biome 5
    /// and biome 8 the sample reads 6.5, which is a biome nobody defined. Keep
    /// IDs on one layer, or carry them as a set of weights, which is what they
    /// should have been anyway. Note that bilinear filtering inside a single
    /// layer already does this to an ID channel at every texel boundary, so
    /// this is a sharper version of a knife the API already had.
    void setSurfaceLayer(int index, const float* data, int width, int height,
                         float originX, float originZ, float metresPerCell,
                         int components = 3);

    /// Single-layer form, unchanged: installs (or releases) layer 0.
    void setSurfaceLayer(const float* data, int width, int height,
                         float originX, float originZ, float metresPerCell,
                         int components = 3);

    int surfaceLayerCount() const { return surfaceLayerCount_; }

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

    /// Current power-of-two zoom of the ring stack, >= 1. Set by update().
    float cellScale() const { return cellScale_; }

    /// How far the surface is visible from `eyeAboveSeaLevel` metres up, for
    /// the configured planet: sqrt(2Rh + h^2). Infinite on a flat world.
    ///
    /// ABOVE SEA LEVEL, not above the ground under your feet. Height above
    /// ground is the right measure for pixel footprint (see cmCellSizeAA) and
    /// the wrong one for visibility: standing 2 m up on a 3700 m massif you can
    /// see 219 km, not 5 km, because the horizon is set by your distance from
    /// the planet's centre. Using height-above-ground here cut the world off a
    /// few tens of km out and left the terrain ending in a wall.
    ///
    /// It grows as sqrt(h): 5 km from a 2 m eye height at sea level, 195 km
    /// from a 3000 m summit, 2293 km from 400 km up.
    float horizonDistance(float eyeAboveSeaLevel) const {
        if (cfg_.planetRadius <= 0.0f) return std::numeric_limits<float>::infinity();
        const float h = std::max(eyeAboveSeaLevel, 0.0f);
        return std::sqrt(2.0f * cfg_.planetRadius * h + h * h);
    }

    /// Radius the app actually has to supply height data across, world metres.
    ///
    /// NOT the same as farDistance, and conflating them is what made the data
    /// bill quadratic. The rings are a fixed triangle budget, so reaching 524 km
    /// from a 2 m eye height costs nothing to DRAW — but demanding elevation
    /// across 524 km costs a 137,000 sq km field, of which 5 km of radius is
    /// above the horizon and the rest is behind the curve.
    ///
    /// Curvature is what makes the shortfall safe: past this radius a layer's
    /// coverage weight fades out and the surface falls to sea level, and that
    /// ground has already bent below the eye ray, so nothing renders. On a flat
    /// world there is no horizon and this correctly equals farDistance.
    ///
    /// Call it every frame with the current eye height above SEA LEVEL.
    float coverageDistance(float eyeAboveSeaLevel) const {
        const float far = farDistance();
        if (cfg_.planetRadius <= 0.0f) return far;
        // Two tangent lengths, not one. The far bound is where the line of
        // sight grazing the planet meets the highest ground the world contains:
        // an eye at h sees the horizon at horizon(h), and a peak of height H
        // beyond it stays visible for another horizon(H). Anything past their
        // sum is behind the curve whatever its elevation.
        //
        // maxHeight_ comes from the layers actually loaded, so a world of
        // rolling hills asks for less than one with 8 km peaks, automatically.
        const float peak = std::max(maxHeight_ - cfg_.seaLevel, 0.0f);
        return std::min(far, horizonDistance(eyeAboveSeaLevel) + horizonDistance(peak));
    }

    /// Half-extent of the coarsest ring, world metres — how far the terrain
    /// reaches from the camera RIGHT NOW. Callers must keep height data
    /// covering at least this radius, and a camera far plane past it.
    float farDistance() const {
        return cfg_.cellSize * cellScale_ * static_cast<float>(cfg_.resolution / 2)
             * std::exp2(static_cast<float>(cfg_.levels - 1));
    }

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

    // Power-of-two zoom of the ring stack, chosen per update from altitude.
    float cellScale_ = 1.0f;

    // Material uniforms
    float rockAlbedo_[3]  = {0.246f, 0.232f, 0.221f};
    float rockRoughness_  = 0.88f;
    float snowAlbedo_[3]  = {0.760f, 0.790f, 0.830f};
    float snowRoughness_  = 0.62f;
    float sandAlbedo_[3]  = {0.480f, 0.430f, 0.330f};
    float sandRoughness_  = 0.94f;
    float grassAlbedo_[3] = {0.180f, 0.235f, 0.128f};
    float grassRoughness_ = 0.97f;
    float forestAlbedo_[3] = {0.105f, 0.205f, 0.098f};   // L0 canopy tint colour
    float forestTint_      = 0.0f;                        // off until setForest()

    struct SurfaceLayer {
        std::vector<float> data;
        int width = 0;
        int height = 0;
        float originX = 0.0f;
        float originZ = 0.0f;
        float metresPerCell = 1.0f;
        bool present = false;
    };
    // Same count as the height stack: a control layer without a height layer
    // under it has nothing to describe, and the reverse is the gap this exists
    // to close.
    SurfaceLayer surf_[kMaxLayers];
    int surfaceLayerCount_ = 0;

    void pushSurfaceUniforms();
};

} // namespace bro::scene
