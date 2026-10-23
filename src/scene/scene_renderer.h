#pragma once

// SceneRenderer: the backend-neutral half of drawing a SceneGraph in 3D —
// the render settings JS configures, light collection, frustum culling and
// the shadow planner. Everything that touches the GPU lives in the
// Vulkan renderer (vulkan/frame/scene_vk_bridge.h and the passes it runs), which
// reads its settings from here every frame.

#include "render/layer_image.h"
#include "scene/exposure.h"
#include "scene/world/atmosphere.h"
#include "scene/world/clouds.h"
#include "scene/nodes/light_node.h"
#include "scene/world/shadow_casters.h"
#include "scene/world/shadow_pages.h"
#include "scene/world/shadow_plan.h"
#include "scene/nodes/plant_movers.h"
#include "scene/world/wind_field.h"

#include <bromath/aabb.h>
#include <bromath/frustum.h>

#include <algorithm>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace bro::render { class VulkanContext; }

namespace bro::scene {

namespace vk { class SceneVkBridge; struct AtmosphereTables; struct ProbeGridTables; }

class SceneGraph;
class SceneNode;
class MeshNode;
class InstancedMeshNode;

/// Per-frame frustum-culling counters, reset at the top of every render3D().
/// "Drawn" counts nodes submitted to a pass (including nodes without valid
/// bounds, which always draw); "culled" counts nodes skipped by a frustum
/// test. Shadow counts are per caster x shadow view (a rect of shadow pages
/// drawn this frame: a caster drawn into two views and culled from one
/// contributes drawn+=2, culled+=1) and come from the shadow pass.
struct CullStats {
    int meshDrawn = 0,       meshCulled = 0;
    int instancedDrawn = 0,  instancedCulled = 0;
    int splatDrawn = 0,      splatCulled = 0;
    int particlesDrawn = 0,  particlesCulled = 0;
    int billboardsDrawn = 0, billboardsCulled = 0;
    int decalsDrawn = 0,     decalsCulled = 0;
    int shadowDrawn = 0,     shadowCulled = 0;
    // The cached shadow pages (world/shadow_pages.h): pages the newest
    // marking named, those of them sampled this frame, pages redrawn into
    // the cache, overlays drawn over it, pages wanted and left for later
    // frames, pages taken from others, and the views (rects) drawn.
    int shadowPagesNeeded = 0, shadowPagesResident = 0, shadowPagesDrawn = 0, shadowPagesDynamic = 0;
    int shadowPagesWanted = 0, shadowPagesEvicted = 0, shadowViews = 0, shadowPagesBuilding = 0;
    // The turf's surface layers (surface_layers.h): tiles the camera needs,
    // those not filled at the current generation, fills issued; and the
    // atlas's shortfall (SurfaceLayerStats::worstError: 1 when none).
    int groundLayerNeeded = 0, groundLayerMissing = 0, groundLayerFills = 0;
    float groundLayerShortfall = 1.0f;
    // The land's tiles made this frame (land.stats() has the rest).
    int landFills = 0;
    // The world service (world/world_service.h): this frame's budget (GPU
    // ms from the measured slack under frameTargetMs), the cost of the jobs
    // it granted, how many it granted and left queued, and the largest
    // error left queued (pixels; 0 when nothing waits).
    float worldBudgetMs = 0.0f, worldSpentMs = 0.0f;
    int worldJobs = 0, worldQueued = 0;
    float worldWorstError = 0.0f;
    // The exposure (scene/exposure.h): the applied EV100 (before the
    // compensation), the metered one and the scale the tonemap multiplied
    // by, as of the newest frame the GPU finished (a frame or two behind).
    float exposureEv = 0.0f, meteredEv = 0.0f, exposureScale = 1.0f;
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
    /// GPU milliseconds of the last render3D (timestamp queries), waiting for
    /// it; -1 when nothing was rendered or timed since the previous call.
    /// Not `wait`: the oldest render finished and not read yet, -1 for none.
    double takeGpuMs(bool wait = true);
    /// Every scene renderer's renders from now on time their passes
    /// (perf.gpuPasses(); nothing is stamped before the first call).
    static void timeGpuPasses();
    /// The last timed render's GPU milliseconds pass by pass, added into
    /// `passes` (by name), and its async compute stream's into `compute`
    /// (`computed` set when it had one). False when no render with its
    /// passes timed has been read.
    bool gpuPasses(std::vector<std::pair<std::string, double>>& passes,
                   std::vector<std::pair<std::string, double>>& compute, bool& computed);

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

    void setToneMap(ToneMap mode, float gamma) {
        toneMap_ = mode; gamma_ = gamma;
    }
    ToneMap toneMap() const { return toneMap_; }
    float gamma() const { return gamma_; }

    /// The exposure (scene/exposure.h): a fixed EV, or metered from each
    /// frame's luminance with eye adaptation.
    void setExposure(const ExposureSettings& e) { exposure_ = e; }
    const ExposureSettings& exposureSettings() const { return exposure_; }
    /// Scene time passed (the scene's tick): the eye adapts over it.
    void advanceExposureTime(float dt) { pendingExposureDt_ += std::max(dt, 0.0f); }
    /// The scene time this render adapts over: what passed since the last render.
    float exposureDt() const { return exposureDt_; }

    /// The flat ambient (without the atmosphere and an environment).
    void setAmbient(float r, float g, float b) {
        ambientColor_[0] = r; ambientColor_[1] = g; ambientColor_[2] = b;
    }
    const float* ambient() const { return ambientColor_; }

    /// The light the ground throws back up: the albedo of a level ground lit
    /// by the sun and the sky, which lights every surface's lower hemisphere
    /// on top of the environment's (a panorama's ground is rarely the scene's,
    /// and a sky-only one has none). Under a high sun this is most of the
    /// light in the shade on anything upright: a trunk, a wall, a leaf's
    /// underside. 0, 0, 0 (the default): none.
    void setGroundBounce(float r, float g, float b) {
        groundBounce_[0] = std::max(r, 0.0f); groundBounce_[1] = std::max(g, 0.0f); groundBounce_[2] = std::max(b, 0.0f);
    }
    const float* groundBounce() const { return groundBounce_; }

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
    float tiltShiftFocusCenter() const { return tiltFocusCenter_; }
    float tiltShiftFocusWidth() const { return tiltFocusWidth_; }
    float tiltShiftFeather() const { return tiltFeather_; }
    float tiltShiftStrength() const { return tiltStrength_; }
    float tiltShiftSaturation() const { return tiltSaturation_; }
    float tiltShiftContrast() const { return tiltContrast_; }

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

    /// The probe grid (docs/scene-world-architecture.md §7): irradiance
    /// probes about the eye, tracing a world made for them (the ground's
    /// height, the plants' crowns as a medium, the sun through it).
    void setProbeGrid(bool enabled) { probeGrid_ = enabled; }
    bool probeGridEnabled() const { return probeGrid_; }
    /// The probe grid's world of the last render (waits for the GPU).
    bool readProbeGrid(vk::ProbeGridTables& out);

    /// Screen-space reflections on opaque surfaces. Runs right after the
    /// opaque passes, SSAO and decals (before translucents): a
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

    /// Dither the tonemapped image as it is written at 8 bits a channel (on
    /// by default): a triangular noise of one level, fixed per pixel, so a
    /// slow gradient (a sky, haze) does not band into plateaus. Off gives
    /// the exact quantised values, for tests that read single pixels.
    void setDither(bool enabled) { ditherEnabled_ = enabled; }
    bool ditherEnabled() const { return ditherEnabled_; }

    /// The frame's GPU target in milliseconds (2.78 by default: 360 fps on
    /// the 4090): the world service (world/world_service.h) spends what the
    /// frame leaves under it on world tiles, and only its floor when the
    /// frame runs over it.
    void setFrameTargetMs(float ms) { frameTargetMs_ = ms > 0.0f ? ms : 2.78f; }
    float frameTargetMs() const { return frameTargetMs_; }

    void setWind(float dirX, float dirY, float dirZ,
                 float strength, float frequency) {
        windDir_[0] = dirX; windDir_[1] = dirY; windDir_[2] = dirZ;
        windStrength_ = strength;
        windFreq_ = frequency;
    }
    void advanceWindTime(float dt) { windTime_ += static_cast<double>(dt); }
    void resetWindTime() { windTime_ = 0.0; }
    float windTime() const { return static_cast<float>(windTime_); }
    double windTimeExact() const { return windTime_; }
    /// The bodies pushing through shrubs and low foliage (plant_movers.h).
    PlantMovers& plantMovers() { return plantMovers_; }
    const PlantMovers& plantMovers() const { return plantMovers_; }
    /// The oscillator clock: the wind clock modulo kWindClockPeriod, reduced
    /// in double (the camera block's windParams.w).
    float windClock() const { return static_cast<float>(std::fmod(windTime_, kWindClockPeriod)); }

    /// The wind field vegetation bends with (wind_field.h). It blows along
    /// setWind's direction; a light breeze until set.
    void setWindField(const WindFieldParams& p) { windField_ = p; }
    const WindFieldParams& windFieldParams() const { return windField_; }
    WindFieldState windField() const { return windFieldState(windDir_, windField_, windTime()); }
    /// How far a plant `height` tall (node units) under `world` can sway:
    /// the culling pad of a node carrying vegetation records (0 in a calm).
    float vegetationReach(float height, const bromath::Mat4& world) const;

    /// The side of the receiver's PCF grid: 1 (one hardware-bilinear tap), 3
    /// (3x3 taps, a 4-texel tent — default) or 5 (5x5 taps, softer). The
    /// shadow maps' resolution is not set: their pages are a texel a pixel
    /// wherever they are sampled (world/shadow_pages.h).
    void setShadowQuality(int pcfTaps) { shadowPCFTaps_ = (pcfTaps >= 5) ? 5 : (pcfTaps >= 3) ? 3 : 1; }
    int shadowPCFTaps() const { return shadowPCFTaps_; }

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

    /// Image-based lighting from an equirect HDR (any format broimage
    /// decodes as float). The panorama is decoded here and kept as RGBA half
    /// floats until the GPU side takes it (takeEnvironmentPixels), which
    /// bakes the radiance cube, its irradiance and its GGX prefilter on the
    /// next frame (scene/vulkan/lighting/scene_environment.h). The lit shaders then
    /// use them for their ambient and the sky shows the cube when the
    /// atmosphere is off. False (and the previous environment kept) when the
    /// file does not decode or is larger than the device's image limit; an
    /// empty path clears and returns true. A bake that still fails (out of
    /// device memory) clears the environment, so hasEnvironment() turns false.
    bool loadEnvironment(const std::string& hdrPath);
    void clearEnvironment();
    bool hasEnvironment() const { return !envPath_.empty(); }
    const std::string& environmentPath() const { return envPath_; }
    /// Moves whenever the environment is loaded or cleared.
    uint64_t environmentGeneration() const { return envGeneration_; }
    int environmentWidth() const { return envWidth_; }
    int environmentHeight() const { return envHeight_; }
    /// The decoded panorama (RGBA16F, top row first), handed over once:
    /// empty after the first call per load.
    std::vector<uint16_t> takeEnvironmentPixels() { return std::move(envPixels_); }

    /// The atmosphere (scene/world/atmosphere.h): with it on, the sky, the
    /// air in front of every surface and the sky's light on them are its
    /// (scene/vulkan/lighting/scene_atmosphere.h).
    void setAtmosphere(const AtmosphereParams& a) { atmosphere_ = a; }
    const AtmosphereParams& atmosphere() const { return atmosphere_; }

    /// The cloud deck (scene/world/clouds.h; vulkan/sky/scene_clouds.h):
    /// drawn with the atmosphere on. Each call is a new generation (its
    /// cover baked again).
    void setClouds(CloudSettings c) {
        c.generation = clouds_.generation + 1;
        clouds_ = std::move(c);
    }
    const CloudSettings& clouds() const { return clouds_; }
    /// What the GPU side made of it (written as it renders).
    const CloudStats& cloudStats() const { return cloudStats_; }
    void setCloudStats(const CloudStats& s) { cloudStats_ = s; }
    /// Tables of the atmosphere, read back from the GPU (waits for the last
    /// render): bro_atmosphere_test's check of them against a reference.
    /// False when nothing has been rendered with the atmosphere on.
    bool readAtmosphereTables(vk::AtmosphereTables& out);

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

    /// The place frame's origin on the body (SceneGraph::placeOrigin), and a
    /// box of the place frame moved onto the body: what is keyed on the
    /// world (the shadow pages) takes the nodes' bounds that way.
    bromath::Vec3 placeOriginF() const;
    bromath::AABB3 bodyBox(const bromath::AABB3& b) const;
    std::optional<bromath::AABB3> bodyBox(const std::optional<bromath::AABB3>& b) const;

    /// Camera-frustum test for the forward passes: true when the node is
    /// provably outside this frame's view and safe to skip. Never true while
    /// culling is disabled or when the node has no valid bounds.
    bool cameraCulled(SceneNode* n) const;
    /// This frame's camera frustum while culling is active, else null.
    const bromath::Frustum* cullingFrustum() const { return cullingActive_ ? &cameraFrustum_ : nullptr; }

    const float* windDir() const { return windDir_; }
    float windStrength() const { return windStrength_; }
    float windFrequency() const { return windFreq_; }

    /// This frame's lights (at most 32, root-attached and visible), or the
    /// implicit sun when the scene declares none. The shadow plan's per-light
    /// arrays are indexed like this list.
    const std::vector<LightNode*>& frameLights() const { return frameLights_; }

    /// This frame's shadow plan (see shadow_plan.h).
    const ShadowPlan& shadowPlan() const { return shadowPlan_; }
    /// The cached shadow pages' policy: the world service's shadow jobs are
    /// its wants, and its grants go back to it (vk::SceneShadowPages).
    ShadowPages& shadowPages() { return shadowPages_; }
    /// How node `nodeId` casts this frame (ShadowMotion; Still for a node
    /// the planner did not meet).
    ShadowMotion shadowMotion(uint32_t nodeId) const;
    /// Every cached shadow page is redrawn (their depth is gone).
    void invalidateShadowPages() { shadowPages_.invalidateAll(); }

private:
    // Rebuilds frameLights_ (see frameLights()).
    void collectLights(std::vector<LightNode*>& out) const;

    // Implicit directional sun used when the scene declares no lights, so
    // meshes are never pitch black. Per-renderer (configured once in the
    // constructor, never mutated afterwards).
    LightNode implicitSun_;
    std::vector<LightNode*> frameLights_;

    // --- Shadow planner (scene_renderer_shadow.cpp) ---
    // Meets the frame's casters (their motion, and what their changes
    // invalidate), sets the shadowed lights' maps, hands the paging the
    // marking passes' requests and plans the frame (shadowPlan_ and the page
    // counters of cullStats_). The Vulkan shadow pass draws what it plans and
    // counts the casters it draws and culls.
    void prepareShadows(const std::vector<LightNode*>& lights);
    // The casters (shadowMotion_, the casters' states, the overlays of what
    // moves) and their bounds' union, the sun's depth range.
    void gatherShadowCasters(bromath::AABB3& casterBounds);
    /// The bounds of what the camera sees out to `reach` (0: its far plane).
    bromath::AABB3 seenBounds(float reach) const;


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
    ExposureSettings exposure_;
    float pendingExposureDt_ = 0.0f;   // scene time since the last render
    float exposureDt_ = 0.0f;          // this render's
    float gamma_ = 2.2f;
    float ambientColor_[3] = {0.03f, 0.03f, 0.03f};
    float groundBounce_[3] = {0.0f, 0.0f, 0.0f};

    // Wind sway (vertex shader displacement)
    float windDir_[3] = {1.0f, 0.0f, 0.0f};
    float windStrength_ = 0.0f;
    float windFreq_ = 1.5f;
    double windTime_ = 0.0;   // double: the oscillators' phases stay exact after hours
    WindFieldParams windField_;
    PlantMovers plantMovers_;

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
    bool  probeGrid_     = false;
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
    bool  ditherEnabled_ = true;
    float frameTargetMs_ = 2.78f;

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
    int shadowPCFTaps_ = 3;       // PCF grid side: 1, 3 or 5 (see setShadowQuality)
    ShadowPages shadowPages_;
    ShadowCasters shadowCasterStates_;
    std::vector<uint64_t> shortSerials_;   // (a plants node's plans out of room, reused)
    ShadowPlan shadowPlan_;
    // This frame's casters that are not still (ShadowMotion), by node id.
    std::unordered_map<uint32_t, ShadowMotion> shadowMotion_;

    // --- Environment state ---
    std::string envPath_;
    std::vector<uint16_t> envPixels_;
    int envWidth_ = 0;
    int envHeight_ = 0;
    uint64_t envGeneration_ = 0;
    float  envIntensity_ = 1.0f;
    float  envRotation_ = 0.0f;
    AtmosphereParams atmosphere_;
    CloudSettings clouds_;
    CloudStats cloudStats_;
    StarfieldParams starfield_;
};

} // namespace bro::scene
