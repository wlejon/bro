#pragma once

// SceneRenderer: the backend-neutral half of drawing a SceneGraph in 3D —
// the render settings JS configures, light collection, frustum culling and
// the shadow-tile planner. Everything that touches the GPU lives in the
// Vulkan renderer (vulkan/scene_vk_bridge.h and the passes it runs), which
// reads its settings from here every frame.

#include "render/layer_image.h"
#include "scene/atmosphere.h"
#include "scene/light_node.h"

#include <bromath/aabb.h>
#include <bromath/frustum.h>

#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace bro::render { class VulkanContext; }

namespace bro::scene {

namespace vk { class SceneVkBridge; }

class SceneGraph;
class SceneNode;
class MeshNode;
class InstancedMeshNode;

/// Per-frame frustum-culling counters, reset at the top of every render3D().
/// "Drawn" counts nodes submitted to a pass (including nodes without valid
/// bounds, which always draw); "culled" counts nodes skipped by a frustum
/// test. Shadow counts are per caster x atlas tile (a caster planned into two
/// cascades and skipped in one contributes drawn+=2, culled+=1) and come from
/// the shadow-tile planner (see planShadowTiles).
struct CullStats {
    int meshDrawn = 0,       meshCulled = 0;
    int instancedDrawn = 0,  instancedCulled = 0;
    int splatDrawn = 0,      splatCulled = 0;
    int particlesDrawn = 0,  particlesCulled = 0;
    int billboardsDrawn = 0, billboardsCulled = 0;
    int decalsDrawn = 0,     decalsCulled = 0;
    int shadowDrawn = 0,     shadowCulled = 0;
    // Shadow-tile cache counters: tiles allocated this frame, tiles that need
    // re-rendering, and tiles whose previous content is still valid. A cached
    // tile contributes nothing to shadowDrawn/shadowCulled.
    int shadowTilesTotal = 0, shadowTilesRendered = 0, shadowTilesCached = 0;
};

class SceneRenderer {
public:
    explicit SceneRenderer(SceneGraph& graph);
    ~SceneRenderer();

    SceneRenderer(const SceneRenderer&) = delete;
    SceneRenderer& operator=(const SceneRenderer&) = delete;

    /// Render all 3D content into the renderer's output image. Called from
    /// SceneGraph::render(). Without a Vulkan context (a CPU-only process)
    /// it logs once and draws nothing.
    void render3D();

    /// True if any 3D content was rendered this frame.
    bool hasMeshContent() const { return hasMeshContent_; }

    /// The frame's LDR output for the compositor, or an empty image when
    /// there is no 3D content. Valid until the next render3D().
    render::LayerImage outputImage() const;

    /// Nodes were destroyed: drop the GPU resources held for them (deferred
    /// until the GPU is done with frames that used them).
    void releaseNodes(std::span<const uint32_t> ids);

    static void setDefaultVulkanContext(render::VulkanContext* ctx) { defaultVulkanContext_ = ctx; }
    static render::VulkanContext* defaultVulkanContext() { return defaultVulkanContext_; }
    vk::SceneVkBridge* vkBridge() const { return vkBridge_.get(); }

    /// Read RGBA8 pixels of the LDR output (top-down row order).
    std::vector<uint8_t> readTonemapPixelsRGBA(int& outW, int& outH);

    // --- Render settings (see SceneGraph for API docs) ---

    void setFog(float start, float end, float r, float g, float b) {
        fogStart_ = start; fogEnd_ = end;
        fogColor_[0] = r; fogColor_[1] = g; fogColor_[2] = b;
    }
    float fogStart() const { return fogStart_; }
    float fogEnd() const { return fogEnd_; }
    const float* fogColor() const { return fogColor_; }

    /// Exponential-squared + height fog. `density` > 0 selects this mode
    /// over the linear start/end ramp (factor = 1 - exp(-(density*d)^2),
    /// d = camera distance past `startDistance`). `heightFalloff` > 0
    /// scales density by exp(-heightFalloff * worldY) per fragment, so low
    /// geometry sits deeper in fog. Color is shared with setFog. Pass
    /// density 0 to fall back to the linear mode (or fully off).
    void setFogExp(float density, float heightFalloff, float startDistance) {
        fogDensity_       = density < 0.0f ? 0.0f : density;
        fogHeightFalloff_ = heightFalloff < 0.0f ? 0.0f : heightFalloff;
        fogStartDist_     = startDistance < 0.0f ? 0.0f : startDistance;
    }
    float fogDensity() const { return fogDensity_; }
    float fogHeightFalloff() const { return fogHeightFalloff_; }
    float fogStartDist() const { return fogStartDist_; }

    enum class ToneMap : uint8_t { Linear, Reinhard, ACES };

    void setToneMap(ToneMap mode, float exposure, float gamma) {
        toneMap_ = mode; exposure_ = exposure; gamma_ = gamma;
    }
    ToneMap toneMap() const { return toneMap_; }
    float exposure() const { return exposure_; }
    float gamma() const { return gamma_; }

    void setAmbient(float r, float g, float b) {
        ambientColor_[0] = r; ambientColor_[1] = g; ambientColor_[2] = b;
    }

    /// The solar irradiance the atmosphere integrates with: the explicit
    /// atmosphere sun colour, else the brightest directional light.
    const float* effectiveSunColor() const {
        return atmosphere_.sunColorExplicit ? atmosphere_.sunColor
                                            : sunIrradiance_;
    }

    /// The ambient the lit shaders get. With the atmosphere on this is the
    /// sky's own irradiance at the camera's altitude (refreshed per frame by
    /// updateSkyAmbient), so a surface is lit by the sky it is under; with it
    /// off it is whatever setAmbient was given.
    const float* effectiveAmbient() const {
        return atmosphere_.enabled ? skyAmbient_ : ambientColor_;
    }

    void setTiltShift(bool enabled, float focusCenter, float focusWidth,
                      float feather, float strength, float saturation,
                      float contrast) {
        tiltEnabled_     = enabled;
        tiltFocusCenter_ = focusCenter;
        tiltFocusWidth_  = focusWidth;
        tiltFeather_     = feather;
        tiltStrength_    = strength;
        tiltSaturation_  = saturation;
        tiltContrast_    = contrast;
    }
    bool tiltShiftEnabled() const { return tiltEnabled_; }

    void setBloom(bool enabled, float threshold, float intensity, float strength) {
        bloomEnabled_   = enabled;
        bloomThreshold_ = threshold;
        bloomIntensity_ = intensity;
        bloomStrength_  = strength;
    }
    bool bloomEnabled() const { return bloomEnabled_; }
    float bloomThreshold() const { return bloomThreshold_; }
    float bloomIntensity() const { return bloomIntensity_; }
    float bloomStrength() const { return bloomStrength_; }

    /// Screen-space ambient occlusion. Computed at half-res from the
    /// resolved opaque depth (hemisphere kernel + rotation noise, separable
    /// blur) and applied to the opaque surfaces' indirect (ambient + probe)
    /// light only, before translucents draw. `radius` is the world-space
    /// hemisphere radius, `intensity` scales how dark occlusion gets (0..1+,
    /// 1 = full AO), `bias` is the depth acceptance offset that suppresses
    /// self-occlusion acne.
    void setSSAO(bool enabled, float radius, float intensity, float bias) {
        ssaoEnabled_   = enabled;
        ssaoRadius_    = radius > 0.0f ? radius : 0.5f;
        ssaoIntensity_ = intensity < 0.0f ? 0.0f : intensity;
        ssaoBias_      = bias;
    }
    bool ssaoEnabled() const { return ssaoEnabled_; }
    float ssaoRadius() const { return ssaoRadius_; }
    float ssaoIntensity() const { return ssaoIntensity_; }
    float ssaoBias() const { return ssaoBias_; }

    /// Screen-space reflections on opaque surfaces. Runs right after the
    /// opaque passes and SSAO (before decals and translucents): a
    /// full-screen pass ray-marches the resolved opaque depth along the
    /// reflected view ray (`steps` linear steps over `maxDistance` world
    /// units + binary refine, `thickness` view-space acceptance) and blends
    /// the hit pixel's HDR color over the surface weighted by its
    /// reflectance * `intensity`, faded near screen borders (`edgeFade`,
    /// fraction of the viewport) and for camera-facing rays. Misses change
    /// nothing. Perspective and ortho cameras both supported.
    void setSSR(bool enabled, float maxDistance, int steps, float thickness,
                float intensity, float edgeFade) {
        ssrEnabled_     = enabled;
        ssrMaxDistance_ = maxDistance > 0.0f ? maxDistance : 30.0f;
        ssrSteps_       = steps < 4 ? 4 : (steps > 256 ? 256 : steps);
        ssrThickness_   = thickness > 0.0f ? thickness : 0.3f;
        ssrIntensity_   = intensity < 0.0f ? 0.0f : intensity;
        ssrEdgeFade_    = edgeFade < 0.0f
                            ? 0.0f : (edgeFade > 0.5f ? 0.5f : edgeFade);
    }
    bool ssrEnabled() const { return ssrEnabled_; }
    float ssrMaxDistance() const { return ssrMaxDistance_; }
    int ssrSteps() const { return ssrSteps_; }
    float ssrThickness() const { return ssrThickness_; }
    float ssrIntensity() const { return ssrIntensity_; }
    float ssrEdgeFade() const { return ssrEdgeFade_; }

    /// Depth-based depth-of-field, applied on the HDR image before bloom +
    /// tonemap. Geometry within focusDistance +/- focusRange (eye-space
    /// distance) stays sharp; the circle of confusion ramps to fully
    /// blurred by +/- 2*focusRange. `maxBlur` is the Gaussian radius in
    /// half-res texels (blur strength of the fully-defocused image).
    void setDepthOfField(bool enabled, float focusDistance, float focusRange,
                         float maxBlur) {
        dofEnabled_       = enabled;
        dofFocusDistance_ = focusDistance > 0.0f ? focusDistance : 10.0f;
        dofFocusRange_    = focusRange > 0.0f ? focusRange : 5.0f;
        dofMaxBlur_       = maxBlur > 0.0f ? maxBlur : 4.0f;
    }
    bool depthOfFieldEnabled() const { return dofEnabled_; }
    float depthOfFieldFocusDistance() const { return dofFocusDistance_; }
    float depthOfFieldFocusRange() const { return dofFocusRange_; }
    float depthOfFieldMaxBlur() const { return dofMaxBlur_; }

    /// Load a 3D color-grading LUT from a horizontal strip image (`size`
    /// tiles of size x size laid out left to right; tile index = blue,
    /// tile x = red, tile y = green, all increasing top-down/left-right —
    /// the standard neutral-strip convention, e.g. a 16^3 LUT is a 256x16
    /// image). size 0 infers the cube side from the image height. The LUT
    /// is applied AFTER tonemapping + gamma, sampled trilinearly from a 3D
    /// texture; `amount` lerps between ungraded and graded (1 = full).
    /// Returns false (and clears nothing) on decode or layout failure.
    bool loadColorLUT(const std::string& path, int size, float amount);
    void clearColorLUT();
    bool hasColorLUT() const { return lutSize_ > 0 && !lutVoxels_.empty(); }
    void setColorLUTAmount(float a) { lutAmount_ = a < 0.0f ? 0.0f : a; }
    int colorLUTSize() const { return lutSize_; }
    float colorLUTAmount() const { return lutAmount_; }
    const std::vector<uint8_t>& colorLUTVoxels() const { return lutVoxels_; }
    /// Moves whenever the LUT's voxels change (the renderer re-uploads).
    uint64_t colorLUTGeneration() const { return lutGeneration_; }

    /// FXAA 3.11 (quality preset) on the final LDR image — always the last
    /// pass in the post stack. Complements MSAA: MSAA resolves geometry
    /// edges in HDR, FXAA additionally smooths shader/specular/post-pass
    /// aliasing on the LDR result; both can be enabled together.
    void setFXAA(bool enabled) { fxaaEnabled_ = enabled; }
    bool fxaaEnabled() const { return fxaaEnabled_; }

    void setWind(float dirX, float dirY, float dirZ,
                 float strength, float frequency) {
        windDir_[0] = dirX; windDir_[1] = dirY; windDir_[2] = dirZ;
        windStrength_ = strength;
        windFreq_ = frequency;
    }
    void advanceWindTime(float dt) { windTime_ += dt; }
    void resetWindTime() { windTime_ = 0.0f; }
    float windTime() const { return windTime_; }

    /// `atlasSize` is the side of the square depth atlas (default 4096).
    /// `pcfTaps` is the side of the receiver's PCF grid: 1 (one hardware-
    /// bilinear tap), 3 (3x3 taps, a 4-texel tent — default) or 5 (5x5 taps,
    /// softer). Tile size is NOT set here: the atlas is cut into a 1x1, 2x2
    /// or 4x4 grid per frame from how many tiles the lights ask for, so one
    /// sun over an orthographic camera gets the whole atlas.
    void setShadowQuality(int atlasSize, int pcfTaps) {
        const int size = atlasSize > 0 ? atlasSize : 4096;
        if (size != shadowAtlasSize_) {
            // Every tile's texels move with the atlas: nothing cached survives.
            invalidateShadowCache();
            shadowAtlasNeedsClear_ = true;
        }
        shadowAtlasSize_ = size;
        shadowPCFTaps_ = (pcfTaps >= 5) ? 5 : (pcfTaps >= 3) ? 3 : 1;
    }
    int shadowAtlasSize() const { return shadowAtlasSize_; }
    int shadowPCFTaps() const { return shadowPCFTaps_; }

    /// Static shadow-tile cache (default on). When enabled, an atlas tile is
    /// only re-rendered when its content could have changed: the owning
    /// light's projection moved (which for directional cascades includes any
    /// camera motion — the cascade fit follows the camera), or the set of
    /// casters overlapping the tile changed membership or mutated (transform,
    /// geometry, visibility). Tiles overlapping skinned or custom-vertex
    /// casters are permanently dynamic and re-render every frame. Strictly
    /// conservative: pixels are identical with the cache on or off — the
    /// escape hatch exists for debugging and regression bisecting.
    void setShadowCache(bool on) {
        if (on != shadowCacheEnabled_) {
            shadowCacheEnabled_ = on;
            invalidateShadowCache();
        }
    }
    bool shadowCacheEnabled() const { return shadowCacheEnabled_; }

    void setShowLightIcons(bool on) { showLightIcons_ = on; }
    bool showLightIcons() const { return showLightIcons_; }

    /// Frustum culling for the forward + shadow passes. Default on; the
    /// escape hatch exists for debugging and regression bisecting.
    void setFrustumCulling(bool on) { frustumCullingEnabled_ = on; }
    bool frustumCullingEnabled() const { return frustumCullingEnabled_; }

    /// Internal render-resolution multiplier (clamped 0.25-2.0). Every frame
    /// target resizes to canvas*scale on the next frame; the compositor
    /// samples the result at the CSS element box, so layout, picking and
    /// camera aspect are unaffected.
    void setRenderScale(float s) {
        renderScale_ = s < 0.25f ? 0.25f : (s > 2.0f ? 2.0f : s);
    }
    float renderScale() const { return renderScale_; }
    /// Device px per CSS px of the display (the engine's render scale, 2 on
    /// Retina); multiplies renderScale in the render-target size.
    void setDeviceScale(float s) { deviceScale_ = s > 0.0f ? s : 1.0f; }

    /// Scaled render-target size: CSS canvas size * renderScale * deviceScale,
    /// at least 1. The canvas (CSS) size stays the contract for picking,
    /// aspect and compositing.
    int targetWidth() const;
    int targetHeight() const;

    /// MSAA sample count for the HDR 3D passes. 0/1 = off; clamped to what
    /// the device supports for the HDR formats when the targets are
    /// allocated (the stored value is the request, not the clamp).
    void setMSAA(int samples) { msaaSamples_ = samples < 2 ? 0 : samples; }
    int msaaSamples() const { return msaaSamples_; }

    /// Culling counters from the most recent render3D().
    const CullStats& cullStats() const { return cullStats_; }

    // --- Environment ---

    /// Image-based lighting from an equirect HDR. Not implemented on the
    /// Vulkan renderer yet: logs that once and returns false, and the scene
    /// keeps its flat ambient. An empty path clears (and returns true).
    bool loadEnvironment(const std::string& hdrPath);
    void clearEnvironment();
    bool hasEnvironment() const { return false; }
    const std::string& environmentPath() const { return envPath_; }

    void setAtmosphere(const AtmosphereParams& a) {
        atmosphere_ = a;
        skyAmbientCamY_ = 1e30f;   // sun moved: force a re-integration
    }
    const AtmosphereParams& atmosphere() const { return atmosphere_; }

    void  setEnvironmentIntensity(float i) { envIntensity_ = (i < 0.0f) ? 0.0f : i; }
    float environmentIntensity() const { return envIntensity_; }

    void  setEnvironmentRotation(float r) { envRotation_ = r; }
    float environmentRotation() const { return envRotation_; }

    void setStarfield(const StarfieldParams& s) { starfield_ = s; }
    const StarfieldParams& starfield() const { return starfield_; }

    // --- Custom mesh shaders ---

    /// Which mesh pipeline a custom-shader variant targets. Static and
    /// Skinned share mesh.vert/mesh.frag (SKINNED define); Instanced splices
    /// into mesh_instanced.vert.
    enum class CustomShaderTarget : uint8_t { Static, Skinned, Instanced };

    /// Compile a pair of user GLSL chunks (either may be empty) for
    /// `target` to SPIR-V, so setShader can report errors at set time.
    /// Returns false with the compiler log in errOut. `key` is
    /// CustomShaderState::key for the same chunk pair.
    bool compileCustomShader(CustomShaderTarget target,
                             const std::string& key,
                             const std::string& vertexChunk,
                             const std::string& fragmentChunk,
                             std::string& errOut);

    // --- What the GPU passes read each frame ---

    /// Conservative world-space AABB for a cullable node (Mesh incl. skinned,
    /// InstancedMesh, GaussianSplat, Particles3D, Decal). Returns nullopt when
    /// the node has no valid bounds — such nodes draw unconditionally.
    std::optional<bromath::AABB3> nodeWorldBounds(SceneNode* n) const;

    /// Camera-frustum test for the forward passes: true when the node is
    /// provably outside this frame's view and safe to skip. Never true while
    /// culling is disabled or when the node has no valid bounds.
    bool cameraCulled(SceneNode* n) const;
    /// This frame's camera frustum while culling is active, else null.
    const bromath::Frustum* cullingFrustum() const { return cullingActive_ ? &cameraFrustum_ : nullptr; }

    const float* windDir() const { return windDir_; }
    float windStrength() const { return windStrength_; }
    float windFrequency() const { return windFreq_; }

private:
    // This frame's lights (at most 32, root-attached and visible), or the
    // implicit sun when the scene declares none. Rebuilt per frame.
    void collectLights(std::vector<LightNode*>& out) const;

    // Implicit directional sun used when the scene declares no lights, so
    // meshes are never pitch black. Per-renderer (configured once in the
    // constructor, never mutated afterwards).
    LightNode implicitSun_;
    std::vector<LightNode*> frameLights_;

    // --- Shadow-tile planner (scene_renderer_shadow.cpp) ---
    // Decides which lights cast shadows this frame, allocates atlas tiles and
    // computes their world->shadow-clip matrices (prepareShadows), then
    // decides per tile whether its cached content is still valid and which
    // casters a re-render would draw (planShadowTiles), filling the shadow
    // counters of cullStats_. The Vulkan shadow pass does not consume the
    // tile plan yet — it renders one directional map of its own (see
    // vulkan/pass_shadow.h) — so the plan is what the atlas WILL draw.
    void prepareShadows(const std::vector<LightNode*>& lights);
    void planShadowTiles();

    // World-space AABBs of (a) every shadow caster and (b) every lit mesh
    // that receives shadows (casters included). The directional fit clips
    // the camera's visible volume against `receivers` and stretches its
    // depth range over `casters`. Either box may come back empty.
    void computeShadowBounds(bromath::AABB3& casters,
                             bromath::AABB3& receivers) const;

    // Sky irradiance, recomputed from atmosphere_ and the camera altitude.
    // Cached against the inputs it depends on so a static camera under a
    // static sun costs nothing.
    void updateSunIrradiance(const std::vector<LightNode*>& lights);
    void updateSkyAmbient(float camY);

    /// The graph this renderer draws. Outlives the renderer (the graph owns
    /// it by value); nodes/camera/canvas state are read through it.
    SceneGraph& graph_;

    // Render-target settings
    float renderScale_ = 1.0f;   // internal-resolution multiplier
    float deviceScale_ = 1.0f;   // display device px per CSS px
    int msaaSamples_ = 0;        // requested sample count; 0/1 = off

    bool hasMeshContent_ = false;

    // Distance fog. Linear ramp (start/end) or, when fogDensity_ > 0,
    // exponential-squared height fog (see setFogExp).
    float fogStart_ = 0.0f;
    float fogEnd_ = 0.0f;
    float fogColor_[3] = {0.0f, 0.0f, 0.0f};
    float fogDensity_ = 0.0f;
    float fogHeightFalloff_ = 0.0f;
    float fogStartDist_ = 0.0f;

    // Tonemap + exposure
    ToneMap toneMap_ = ToneMap::ACES;
    float exposure_ = 1.0f;
    float gamma_ = 2.2f;
    float ambientColor_[3] = {0.03f, 0.03f, 0.03f};

    // Wind sway (vertex shader displacement)
    float windDir_[3] = {1.0f, 0.0f, 0.0f};
    float windStrength_ = 0.0f;
    float windFreq_ = 1.5f;
    float windTime_ = 0.0f;

    // Editor affordance: render a marker icon per LightNode and include
    // them in raycast results.
    bool showLightIcons_ = false;

    // --- Frustum culling ---
    bool frustumCullingEnabled_ = true;
    // Per-frame state set at the top of render3D(): world-space camera
    // frustum (valid while cullingActive_) + drawn/culled counters.
    bool cullingActive_ = false;
    bromath::Frustum cameraFrustum_;
    CullStats cullStats_;

    std::unique_ptr<vk::SceneVkBridge> vkBridge_;
    static inline render::VulkanContext* defaultVulkanContext_ = nullptr;

    // --- SSAO ---
    bool  ssaoEnabled_   = false;
    float ssaoRadius_    = 0.5f;
    float ssaoIntensity_ = 1.0f;
    float ssaoBias_      = 0.025f;

    // --- SSR ---
    bool  ssrEnabled_     = false;
    float ssrMaxDistance_ = 30.0f;
    int   ssrSteps_       = 48;
    float ssrThickness_   = 0.3f;
    float ssrIntensity_   = 1.0f;
    float ssrEdgeFade_    = 0.1f;

    // --- Depth of field ---
    bool  dofEnabled_       = false;
    float dofFocusDistance_ = 10.0f;
    float dofFocusRange_    = 5.0f;
    float dofMaxBlur_       = 4.0f;

    // --- 3D color-grading LUT ---
    int    lutSize_   = 0;
    float  lutAmount_ = 1.0f;
    std::vector<uint8_t> lutVoxels_;   // size^3 RGBA8, r fastest
    uint64_t lutGeneration_ = 0;

    // --- FXAA ---
    bool  fxaaEnabled_ = false;

    // --- Tilt-shift ---
    bool  tiltEnabled_     = false;
    float tiltFocusCenter_ = 0.5f;
    float tiltFocusWidth_  = 0.12f;
    float tiltFeather_     = 0.25f;
    float tiltStrength_    = 2.0f;
    float tiltSaturation_  = 1.0f;
    float tiltContrast_    = 1.0f;

    // --- Bloom ---
    bool  bloomEnabled_   = false;
    float bloomThreshold_ = 1.0f;
    float bloomIntensity_ = 0.0f;
    float bloomStrength_  = 2.0f;

    // --- Shadow planner state ---
    // Hard cap: 16 atlas tiles. A typical scene budget is 1 directional
    // (1-4 cascades) + a few spots/points; overflow lights go unshadowed.
    static constexpr int kMaxShadowTiles = 16;

    int shadowAtlasSize_ = 4096;
    int shadowPCFTaps_ = 3;       // PCF grid side: 1, 3 or 5 (see setShadowQuality)
    // Atlas grid for the current frame: 1, 2 or 4 tiles per side, chosen by
    // prepareShadows from the tile demand (1 -> whole atlas, <=4 -> quarters,
    // else 16ths). Changing it moves every tile, so it invalidates the cache.
    int shadowGridDim_ = 4;

    // Per-frame shadow data, populated by prepareShadows(). Indexed by slot.
    int   shadowTileCount_ = 0;
    float shadowMatrixCamRel_[kMaxShadowTiles][16] = {};
    float shadowAtlasRect_[kMaxShadowTiles][4]     = {};   // origin.xy, size.xy in [0,1]
    float shadowBias_[kMaxShadowTiles][2]          = {};   // const, normal-bias world units
    // World size of one shadow texel at the receiver: .x constant (ortho
    // directional tiles), .y per metre of light distance (spot/point tiles,
    // whose texels grow with distance).
    float shadowTexelWorld_[kMaxShadowTiles][2]    = {};
    // (near, far, isOrtho) of the tile's projection, so a receiver can turn a
    // world-unit depth bias into [0,1] depth.
    float shadowDepthParams_[kMaxShadowTiles][3]   = {};

    // Per-light shadow slot (-1 if unshadowed). Indexed by light index.
    int lightShadowSlot_[32] = {};
    // For directional CSM: 1..4 cascades, each occupies a contiguous slot.
    int   lightShadowSlotCount_[32] = {};
    // Cascade FAR distances in view space; .x = cascade 0 far, etc.
    float lightCascadeSplit_[32][4] = {};

    // Matrices to render into the atlas (one per tile), world space.
    float shadowRenderMatrix_[kMaxShadowTiles][16] = {};
    // Which light owns each tile, for routing the caster draws.
    LightNode* shadowTileLight_[kMaxShadowTiles] = {};

    // --- Static shadow-tile cache ---
    // One entry per atlas tile, recording what the tile's depth content was
    // planned from: the owning light (by node id — ids are never reused),
    // the exact world->clip matrix, and the ordered (node id, change
    // generation) list of casters that overlapped the tile's frustum,
    // interleaved with (0, listIndex) separators so a caster migrating
    // between caster lists never aliases an unchanged signature. A tile is
    // reused when the current signature is identical — any difference, or
    // any overlapping skinned/custom-vertex caster, re-renders.
    struct ShadowTileCacheEntry {
        bool valid = false;
        uint32_t lightId = 0;
        float lightVP[16] = {};
        std::vector<std::pair<uint32_t, uint64_t>> casters;
    };
    ShadowTileCacheEntry shadowTileCache_[kMaxShadowTiles];
    bool shadowCacheEnabled_ = true;
    // A freshly (re)allocated atlas holds garbage — force one full clear
    // (and thus a full re-render) before any per-tile reuse.
    bool shadowAtlasNeedsClear_ = true;
    void invalidateShadowCache() {
        for (auto& e : shadowTileCache_) e.valid = false;
    }

    // Per-frame shadow caster lists; rebuilt at the top of prepareShadows.
    // Skinned casters deform with the palette; casters whose custom shader
    // has a VERTEX chunk split into the custom lists (sorted by chunk source)
    // so a displaced silhouette can be drawn per group. Fragment-only custom
    // shaders stay in the default lists.
    std::vector<MeshNode*> shadowCasters_;
    std::vector<MeshNode*> shadowSkinnedCasters_;
    std::vector<MeshNode*> shadowCustomCasters_;
    std::vector<MeshNode*> shadowSkinnedCustomCasters_;
    std::vector<InstancedMeshNode*> shadowInstancedCasters_;
    std::vector<InstancedMeshNode*> shadowTubeCasters_;

    // --- Environment state ---
    std::string envPath_;
    float  envIntensity_ = 1.0f;
    float  envRotation_ = 0.0f;
    AtmosphereParams atmosphere_;
    StarfieldParams starfield_;

    // Solar irradiance taken from the scene's brightest directional light,
    // so the atmosphere scatters the same sun the surface is lit by.
    float sunIrradiance_[3] = {3.0f, 2.94f, 2.85f};
    float skyAmbient_[3] = {0.03f, 0.03f, 0.03f};
    float skyAmbientCamY_ = 1e30f;
};

} // namespace bro::scene
