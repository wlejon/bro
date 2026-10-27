#pragma once

// The cloud deck on the GPU (scene/world/clouds.h; docs/clouds-api.js;
// docs/scene-world-architecture.md §13 "volumes"): the world's module, its
// cover, the engine's noise, the march and the shadow.
//
//   - The module's set (CloudShaders, a ModuleShaders set): the cover's
//     bake (sky/clouds_cover.comp), the cache's base (sky/clouds_cache.comp)
//     and the globe's (sky/clouds_globe.comp), each with the world's module in sky/cloud_module.glsl's place and,
//     for a deck set over a land with processes, its grid's layout in
//     ground/land_grid_config.glsl's. Nothing else runs the module.
//   - The noise (sky/clouds_noise.comp): one tile of tiling noise and its
//     mips, made once when a scene first has clouds.
//   - The cover: the module's cloudCover over a cube about the body, baked
//     when the clouds are set (and when the land they read is ready).
//   - The cache (sky/clouds_layout.h): the world's base and the engine's
//     forms' mean extinction over levels of windows about the eye's nadir
//     on a chart fixed to the body, and the depths from each texel toward
//     the sun and up (sky/clouds_light.comp); a level baked whole when its
//     window moves, a share of every level's rows each frame.
//   - The globe (sky/clouds_globe.comp): the whole deck's columns as layers
//     seen from above, on a cube about the body (CL_GLOBE_SIDE), what the
//     march reads from above where the cache's cells are coarse or the
//     pixel wider than the clouds; baked whole when the deck or the time
//     jumps, a slice of its rows each frame, only while the eye is over
//     the deck.
//   - The shadow (sky/clouds_shadow.comp): what the deck leaves of the sun
//     over CL_SHADOW_LEVELS levels about the body's point under the eye,
//     read from the cache every frame the deck is drawn, read by every lit
//     shader (the lighting set's binding 24; scene_lighting.glsl's
//     sceneCloudSun).
//   - The volume (scene_cloud_volume.h): the forms as the march draws them
//     baked into a clipmap of 3D grids about the eye, with a distance to
//     cloud a march may jump by; baked after the cache (PassCloudVolume).
//     The march reads its extinction deep in a cloud.
//   - Every compute pipeline here and the march's are built off the frame
//     thread (AsyncComputePipeline, PipelineVariants' async), the module's
//     set compiled off it (ModuleShaders::start): the deck is neither baked
//     nor drawn until they are in (cloudStats().compiled false meanwhile).
//   - The march (sky/clouds.frag, built in): the cache's base cut into the
//     forms per sample, drawn over the opaque surfaces in the HDR scope
//     (pass_clouds.h).
//
// Nothing of it runs, and nothing is allocated, in a scene without clouds:
// its passes are off, the lighting block says no shadow and binding 24 is a
// texel the shaders never read.

#include "scene/vulkan/frame/scene_module_shaders.h"
#include "scene/vulkan/frame/scene_vk_allocator.h"
#include "scene/vulkan/frame/scene_vk_descriptors.h"
#include "scene/vulkan/shaders/sky/clouds_layout.h"
#include "scene/vulkan/sky/scene_cloud_volume.h"

#include <vulkan/vulkan.h>

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace bro::scene::vk {

struct SceneFrame;
struct SceneGpu;

/// The shaders a deck is drawn with.
class CloudShaders : public ModuleShaders {
public:
    enum Stage : int {
        Cover,    // clouds_cover.comp
        Cache,    // clouds_cache.comp
        Globe,    // clouds_globe.comp
        Heaps,    // clouds_heaps.comp
        View,     // clouds_view.comp
        kStages
    };
    static constexpr BuiltinSceneShader kBuiltin[kStages] = {
        BuiltinSceneShader::CloudsCoverComp, BuiltinSceneShader::CloudsCacheComp, BuiltinSceneShader::CloudsGlobeComp,
        BuiltinSceneShader::CloudsHeapsComp, BuiltinSceneShader::CloudsViewComp};

    /// The set with `module` as cloud_module.glsl and `grid` (or none, "")
    /// as land_grid_config.glsl, compiled off the frame thread (poll).
    void start(const std::string& module, const std::string& grid) {
        Overrides overrides{{"sky/cloud_module.glsl", module}};
        if (!grid.empty()) overrides.push_back({"ground/land_grid_config.glsl", grid});
        ModuleShaders::start(kBuiltin, overrides);
    }

    VkShaderModule operator[](Stage s) const { return module(s); }
    // Started once the modules are made.
    AsyncComputePipeline cover, cache, globe, heaps, view;
};

class SceneClouds {
public:
    bool setup(SceneGpu& gpu);
    void cleanup(SceneGpu& gpu);

    /// The frame begins (after the atmosphere's, before its lighting is
    /// filled): whether the deck is drawn, its module compiled, whether its
    /// cover is to be baked, and where the shadow's levels lie.
    void beginFrame(SceneFrame& frame);
    /// The deck is drawn this frame.
    bool active() const { return on_; }
    /// The deck is drawn or its cover is being baked (a face a frame) this frame.
    bool preparing() const { return on_ || bake_; }
    /// The lighting block's shadow (off: params[0] 0) and the lighting set's
    /// binding 24.
    void fill(CloudShadowGpu& out) const { out = shadowBlock_; }
    VkImageView shadowView() const;

    /// The noise (once), the cover (when it is to be baked) and the
    /// shadow's levels, into the graphics stream outside any rendering,
    /// before anything lit reads them (PassCloudsPrepare).
    void recordPrepare(SceneFrame& frame);
    /// The march, in the HDR scope over the opaque surfaces (PassClouds).
    void recordDraw(SceneFrame& frame);

    /// The cover's cube as baked whole (a debugging read; waits for the
    /// GPU): CL_COVER_SIDE texels a face with its border, six faces, rgba.
    /// False before the cover is baked once.
    bool readCover(SceneGpu& gpu, std::vector<float>& out);
    /// The cover at body direction `up` in a cube readCover gave, bilinear
    /// as the shaders read it (sky/scene_cloud_layer.glsl's cloudCoverAt).
    static void coverAt(const std::vector<float>& cube, const double up[3], float out[4]);

    /// The clouds' block (std140) — CloudsUBO in sky/scene_cloud_layer.glsl.
    struct alignas(16) Block {
        float toBody[3][4];
        float shell[4];
        float params[4];
        float sun[4];
        float shadowBake[3][4];                  // the shadow's levels baked this frame, in order
        float shadowN[CL_SHADOW_LEVELS][4];      // each level's nadir (air space), w its texel (m)
        float shadowE1[CL_SHADOW_LEVELS][4];
        float shadowE2[CL_SHADOW_LEVELS][4];
        float cacheC[4];
        float cacheE1[4];
        float cacheE2[4];
        float cacheNadir[4];
        float cacheLevel[13][4];
        float forms[4];
        float wind[4];
        float heapNadir[4];
        float heapLevel[CL_HEAP_LEVELS][4];
        float data[64][4];
        CloudVolumeGpu volume;
        float viewToWorld[3][4];   // the view's columns (sky/clouds_view.comp)
        float viewInvProj[4][4];
        float viewEye[4];
        float viewSize[4];
    };

    /// The volume about the eye's bakes, into the graphics stream after
    /// recordPrepare (PassCloudVolume), and the frame they went in submitted.
    void recordVolume(SceneFrame& frame);
    void volumeSubmitted(uint64_t ticket) { volume_.submitted(ticket); }

private:
    CloudShaders* shaders(SceneFrame& frame);
    // The shadow's levels this frame about the eye's nadir `n` (axes `e1`,
    // `e2`; air space): which are baked again, and each one's plane.
    void placeShadow(const float n[3], const float e1[3], const float e2[3], float R, const CloudSettings& c);
    bool ensureImages(SceneGpu& gpu);
    bool createNoise(SceneGpu& gpu);
    void recordNoise(SceneFrame& frame);
    VkDescriptorSet writeSet(SceneFrame& frame, bool march, bool bake);
    /// Where the cache's chart and windows lie this frame, and what of it
    /// is baked (beginFrame).
    void placeCache(SceneFrame& frame);
    /// The cache's bakes: its extinction, then its light, over the levels
    /// and rows `bakes_` lists.
    void recordCache(SceneFrame& frame);
    /// The globe's bake: all of it when it is not current, else a slice of
    /// its rows, while the eye is over the deck (sky/clouds_layout.h).
    void recordGlobe(SceneFrame& frame);
    /// The view's columns (sky/clouds_view.comp), each frame they are used
    /// (beginFrame says: block_.viewSize).
    void recordView(SceneFrame& frame);
    /// Where the heaps' map's windows lie this frame (about the drifted
    /// nadir) and what of it is baked (beginFrame, after placeCache); its
    /// bakes (before the cache's).
    void placeHeaps(SceneFrame& frame, const double nadir[3]);
    void recordHeaps(SceneFrame& frame);
    /// Body direction `u` on the cache's chart (m): cloudChartAt's.
    void chartAt(const double u[3], double& x, double& y) const;

    SceneVkDevice* device_ = nullptr;
    VkDescriptorSetLayout setLayout_ = VK_NULL_HANDLE;
    VkPipelineLayout marchLayout_ = VK_NULL_HANDLE;
    VkPipelineLayout computeLayout_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout noiseSetLayout_ = VK_NULL_HANDLE;
    VkPipelineLayout noiseLayout_ = VK_NULL_HANDLE;
    VkShaderModule skyVs_ = VK_NULL_HANDLE;
    VkShaderModule marchFs_ = VK_NULL_HANDLE;
    VkShaderModule marchVolumeFs_ = VK_NULL_HANDLE;   // the debug variant (BRO_CLOUD_VOLUME_DEBUG)
    PipelineVariants marchPipelines_;
    SceneCloudVolume volume_;
    VkShaderModule noiseCs_ = VK_NULL_HANDLE;
    AsyncComputePipeline noisePipeline_;
    VkSampler coverSampler_ = VK_NULL_HANDLE;
    VkSampler noiseSampler_ = VK_NULL_HANDLE;
    VkSampler cacheSampler_ = VK_NULL_HANDLE;
    VkShaderModule lightCs_ = VK_NULL_HANDLE;
    VkShaderModule shadowCs_ = VK_NULL_HANDLE;
    AsyncComputePipeline lightPipeline_;
    VkShaderModule distCs_ = VK_NULL_HANDLE;
    AsyncComputePipeline distPipeline_;
    AsyncComputePipeline shadowPipeline_;
    /// Whether the set's and the engine's pipelines are in (the march's is
    /// asked for as it draws): nothing of the deck is baked or drawn before.
    enum class Readiness { Building, Ready, Failed };
    Readiness pipelinesReady(CloudShaders& set);

    // The module sets, by their sources' hash (-1: did not compile).
    std::vector<std::unique_ptr<CloudShaders>> sets_;
    std::unordered_map<uint64_t, int> byHash_;
    CloudShaders* current_ = nullptr;
    uint64_t currentKey_ = 0;    // its module's and grid layout's hash

    // The noise: a 3D image, its mips, a storage view a mip and a sampled view of all.
    struct Noise {
        VkImage image = VK_NULL_HANDLE;
        uint64_t allocId = 0;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        VkImageView sampled = VK_NULL_HANDLE;
        VkImageView mips[16] = {};
        bool made = false;
    } noise_;
    SceneVkImage cover_, shadow_;
    SceneVkImage coverNone_, shadowNone_, gridNone_;
    SceneVkImage cacheSigma_, cacheLight_, cacheBand_, distA_, distB_, cacheHolds_;
    SceneVkImage globe_;
    SceneVkImage view_;          // the view's columns (sky/clouds_layout.h's CL_VIEW_*)
    // The deck the globe holds (its module, generation; 0 none), the wind's
    // time it was last baked at, and its next slice.
    uint64_t globeKey_ = 0;
    float globeTime_ = 0.0f;
    uint32_t globeFrame_ = 0;
    float eyeRadius_ = 0.0f;     // the eye's distance from the body's centre (m)

    /// A level whose window moved from cell `from` to `to` along `axis`
    /// (0 x, 1 y): the bakes of what that changes (placeCache).
    void moveLevel(int k, int axis, long long from, long long to);

    // The cache's chart (the body's frame) and its levels' windows, as
    // baked; a bake this frame: `levels` levels from `first`, `nx` columns
    // from `x0` and `ny` rows from `y0` every `yStep` (both wrapping about
    // the side), its passes from `pass0` (0 the base, 1 the light, 2 the
    // distance to cloud).
    struct CacheBake {
        int first, levels, x0, nx, y0, ny, yStep, pass0;
        static CacheBake level(int first, int levels) {
            return {first, levels, 0, CL_CACHE_SIDE, 0, CL_CACHE_SIDE, 1, 0};
        }
    };
    std::vector<CacheBake> bakes_;
    bool cacheBaked_ = false;       // the cache holds a deck (its first bake is whole)
    uint32_t cachePending_ = 0;     // levels still to be baked again, a level a frame
    double chartC_[3] = {}, chartE1_[3] = {}, chartE2_[3] = {};
    long long levelCentre_[13][2] = {};
    bool chartValid_ = false;
    uint64_t cacheKey_ = 0;      // the deck the cache holds (its module, generation)
    float cacheTime_ = 0.0f;     // the wind's time it was last baked at
    float cacheSun_[3] = {};     // and the sun (the body's frame)
    uint32_t cacheFrame_ = 0;
    float volumeTime_ = 0.0f;    // the wind's time the volume was last placed at
    uint32_t chartSerial_ = 0;   // moved each time the chart is laid about a new centre

    // The heaps' map (sky/clouds_layout.h): a bake this frame, `levels`
    // levels from `first`, `nx` texels from `x0` of `ny` rows from `y0`
    // (both wrapping about the side).
    struct HeapBake {
        int first, levels, x0, nx, y0, ny;
        static HeapBake level(int first, int levels) { return {first, levels, 0, CL_HEAP_SIDE, 0, CL_HEAP_SIDE}; }
    };
    SceneVkImage heaps_;
    std::vector<HeapBake> heapBakes_;
    long long heapCentre_[CL_HEAP_LEVELS][2] = {};
    bool heapBaked_ = false;        // the map holds the deck's heaps (its first bake is whole)
    uint32_t heapPending_ = 0;      // levels still to be baked again, a level a frame
    uint64_t heapKey_ = 0;          // the deck it holds (its module, generation, the lattice's cell)
    uint32_t heapChart_ = 0;        // the chart's serial it was baked on

    Block block_{};
    CloudShadowGpu shadowBlock_{};
    // The shadow's levels: each one's nadir and axes in the body's frame as
    // it was last baked (so a re-anchor moves none), how many are baked this
    // frame (block_.shadowBake), and what says when the rest are again.
    double shadowLevel_[CL_SHADOW_LEVELS][3][3] = {};
    int shadowBakes_ = 0;
    uint32_t shadowTurn_ = 0;
    bool shadowValid_ = false;
    float shadowTime_ = 0.0f;
    uint64_t shadowKey_ = 0;
    float shadowSun_[3] = {};
    double shadowNadir_[3] = {0.0, 1.0, 0.0};   // the eye's this frame (air space)
    bool on_ = false;
    bool shadowsOn_ = false;
    bool bake_ = false;          // a face of the cover is baked this frame (coverFace_)
    bool coverDone_ = false;     // and it is the cover's last
    bool coverHeld_ = false;     // the cover has been baked whole once (the deck is drawn meanwhile)
    int coverFace_ = 0;          // the next face of the bake under way
    VkImageView bakeGrid_ = VK_NULL_HANDLE;
    uint64_t bakedGeneration_ = 0;
    uint64_t bakedHash_ = 0;
    uint64_t bakedEpoch_ = 0;    // the shader sets' bake epoch it was baked in
    uint64_t bakingKey_ = 0;     // what the bake under way is of (0: none)
    int sliceRun_ = 0;           // the cache's slices made since its last whole change
};

}  // namespace bro::scene::vk
