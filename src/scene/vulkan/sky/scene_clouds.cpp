#include "scene/vulkan/sky/scene_clouds.h"
#include "scene/vulkan/frame/scene_gpu_timer.h"
#include "scene/vulkan/frame/scene_shader_sets.h"

#include "render/vulkan_util.h"
#include "scene/nodes/land_node.h"
#include "scene/nodes/world_module.h"
#include "scene/scene_graph.h"
#include "scene/scene_renderer.h"
#include "scene/vulkan/draw/scene_mesh_drawer.h"
#include "scene/vulkan/frame/scene_defaults.h"
#include "scene/vulkan/frame/scene_frame.h"
#include "scene/vulkan/frame/scene_targets.h"
#include "scene/vulkan/frame/scene_vk_device.h"
#include "scene/vulkan/frame/scene_vk_pipeline.h"
#include "scene/vulkan/frame/scene_vk_shader_compiler.h"
#include "scene/vulkan/lighting/scene_atmosphere.h"
#include "scene/vulkan/shaders/sky/clouds_layout.h"
#include "util/log.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>

namespace bro::scene::vk {

static_assert(sizeof(SceneClouds::Block) ==
              16 * (3 + 3 + 3 + 3 * CL_SHADOW_LEVELS + 4 + CL_CACHE_LEVELS + 2 + 1 + CL_HEAP_LEVELS + CL_MAX_DATA + 5 + CL_VOL_LEVELS + 3 + 4 + 2 + 6));
static_assert(CL_HEAP_SIDE % CL_HEAP_GROUP == 0 && (CL_HEAP_SIDE & (CL_HEAP_SIDE - 1)) == 0);
static_assert(CloudSettings::kMaxData == CL_MAX_DATA);
static_assert(CL_CACHE_LEVELS == 13 && CL_CACHE_SIDE % (CL_CACHE_GROUP * CL_CACHE_SLICES) == 0);

namespace {

constexpr VkFormat kCacheFormat = VK_FORMAT_R16G16B16A16_SFLOAT;
constexpr VkFormat kSigmaFormat = VK_FORMAT_R16G16B16A16_SFLOAT;
constexpr VkFormat kDistFormat = VK_FORMAT_R16_SFLOAT;
constexpr VkFormat kCoverFormat = VK_FORMAT_R16G16B16A16_SFLOAT;
constexpr VkFormat kHeapFormat = VK_FORMAT_R16G16B16A16_SFLOAT;
constexpr VkFormat kShadowFormat = VK_FORMAT_R32_SFLOAT;   // what is left of the sun
constexpr VkFormat kGridFormat = VK_FORMAT_R32G32B32A32_SFLOAT;
// (Half floats: the mips' variance, E[x^2] - E[x]^2, is a small difference.)
constexpr VkFormat kNoiseFormat = VK_FORMAT_R8G8B8A8_UNORM;
constexpr VkShaderStageFlags kStages =
    VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_COMPUTE_BIT;
// The shells' buffer (sky/clouds_layout.h): the head, the heaps as drawn and as listed.
constexpr VkDeviceSize kShellBytes =
    CL_SHELL_HEAD * sizeof(uint32_t) + 2ull * CL_SHELL_MOST * CL_SHELL_VEC4S * 4 * sizeof(float);
constexpr VkDeviceSize kShellHeadBytes = CL_SHELL_HEAD * sizeof(uint32_t);

uint64_t mixBits(uint64_t h, uint64_t v) { return h ^ (v + 0x9E3779B97F4A7C15ull + (h << 6) + (h >> 2)); }

VkPipelineLayout pipelineLayout(VkDevice dev, std::initializer_list<VkDescriptorSetLayout> sets, uint32_t push) {
    const std::vector<VkDescriptorSetLayout> s(sets);
    const VkPushConstantRange range{VK_SHADER_STAGE_COMPUTE_BIT, 0, push};
    VkPipelineLayoutCreateInfo info{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    info.setLayoutCount = static_cast<uint32_t>(s.size());
    info.pSetLayouts = s.data();
    info.pushConstantRangeCount = push ? 1 : 0;
    info.pPushConstantRanges = push ? &range : nullptr;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    if (vkCreatePipelineLayout(dev, &info, nullptr, &layout) != VK_SUCCESS) return VK_NULL_HANDLE;
    return layout;
}

void memoryBarrier(VkCommandBuffer cmd, VkPipelineStageFlags src, VkAccessFlags srcAccess, VkPipelineStageFlags dst,
                   VkAccessFlags dstAccess) {
    VkMemoryBarrier mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    mb.srcAccessMask = srcAccess;
    mb.dstAccessMask = dstAccess;
    vkCmdPipelineBarrier(cmd, src, dst, 0, 1, &mb, 0, nullptr, 0, nullptr);
}

constexpr VkPipelineStageFlags kReaders =
    VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;

}  // namespace

bool SceneClouds::setup(SceneGpu& gpu) {
    device_ = &gpu.device;
    VkDevice dev = gpu.device.device();
    SceneVkDescriptorLayoutBuilder set;
    set.addBinding(CL_B_UBO, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, kStages);
    set.addBinding(CL_B_COVER, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, kStages);
    set.addBinding(CL_B_NOISE, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, kStages);
    set.addBinding(CL_B_DEPTH, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, kStages);
    set.addBinding(CL_B_COVER_OUT, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, kStages);
    set.addBinding(CL_B_SHADOW_OUT, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, kStages);
    set.addBinding(CL_B_LAND_GRID, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, kStages);
    set.addBinding(CL_B_CACHE, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, kStages);
    set.addBinding(CL_B_CACHE_OUT, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, kStages);
    set.addBinding(CL_B_SIGMA, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, kStages);
    set.addBinding(CL_B_SIGMA_OUT, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, kStages);
    set.addBinding(CL_B_BAND, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, kStages);
    set.addBinding(CL_B_BAND_OUT, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, kStages);
    set.addBinding(CL_B_DIST_A, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, kStages);
    set.addBinding(CL_B_DIST_B, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, kStages);
    set.addBinding(CL_B_HOLDS, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, kStages);
    set.addBinding(CL_B_GLOBE, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, kStages);
    set.addBinding(CL_B_GLOBE_OUT, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, kStages);
    set.addBinding(CL_B_VIEW, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, kStages);
    set.addBinding(CL_B_VIEW_OUT, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, kStages);
    set.addBinding(CL_B_VOLUME, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, CL_VOL_LEVELS, kStages);
    set.addBinding(CL_B_VOLUME_SAMPLER, VK_DESCRIPTOR_TYPE_SAMPLER, 1, kStages);
    set.addBinding(CL_B_VOLUME_OUT, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, CL_VOL_LEVELS, kStages);
    set.addBinding(CL_B_VOLUME_DA, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, CL_VOL_LEVELS, kStages);
    set.addBinding(CL_B_VOLUME_DB, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, CL_VOL_LEVELS, kStages);
    set.addBinding(CL_B_VOLUME_CHECK, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, kStages);
    set.addBinding(CL_B_HEAPS, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, kStages);
    set.addBinding(CL_B_HEAPS_OUT, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, kStages);
    set.addBinding(CL_B_SHELLS, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, kStages);
    setLayout_ = set.build(dev);
    SceneVkDescriptorLayoutBuilder noise;
    noise.addBinding(0, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT);
    noise.addBinding(1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT);
    noiseSetLayout_ = noise.build(dev);
    if (!setLayout_ || !noiseSetLayout_) return false;
    marchLayout_ = pipelineLayout(dev, {gpu.defaults.cameraLayout, gpu.defaults.lightingLayout, setLayout_}, 0);
    computeLayout_ = pipelineLayout(dev, {setLayout_}, 8 * sizeof(int32_t));
    noiseLayout_ = pipelineLayout(dev, {noiseSetLayout_}, 2 * sizeof(int32_t));
    skyVs_ = SceneVkShaderCompiler::createBuiltinModule(dev, BuiltinSceneShader::SkyVert);
    marchFs_ = SceneVkShaderCompiler::createBuiltinModule(dev, BuiltinSceneShader::CloudsFrag);
    if (SceneCloudVolume::debugView() > 0)
        marchVolumeFs_ = SceneVkShaderCompiler::createBuiltinModule(dev, BuiltinSceneShader::CloudsVolumeFrag);
    noiseCs_ = SceneVkShaderCompiler::createBuiltinModule(dev, BuiltinSceneShader::CloudsNoiseComp);
    lightCs_ = SceneVkShaderCompiler::createBuiltinModule(dev, BuiltinSceneShader::CloudsLightComp);
    shadowCs_ = SceneVkShaderCompiler::createBuiltinModule(dev, BuiltinSceneShader::CloudsShadowComp);
    distCs_ = SceneVkShaderCompiler::createBuiltinModule(dev, BuiltinSceneShader::CloudsDistComp);
    shellsSortCs_ = SceneVkShaderCompiler::createBuiltinModule(dev, BuiltinSceneShader::CloudsShellsSortComp);
    shellsVs_ = SceneVkShaderCompiler::createBuiltinModule(dev, BuiltinSceneShader::CloudShellsVert);
    shellsFs_ = SceneVkShaderCompiler::createBuiltinModule(dev, BuiltinSceneShader::CloudShellsFrag);
    if (shellsSortCs_ && computeLayout_) shellsSortPipeline_.start(dev, shellsSortCs_, computeLayout_, "clouds shells sort");
    // (Built off the frame thread; the deck is drawn once they are in.)
    if (noiseCs_ && noiseLayout_) noisePipeline_.start(dev, noiseCs_, noiseLayout_, "clouds noise");
    if (lightCs_ && computeLayout_) lightPipeline_.start(dev, lightCs_, computeLayout_, "clouds light");
    if (distCs_ && computeLayout_) distPipeline_.start(dev, distCs_, computeLayout_, "clouds dist");
    if (shadowCs_ && computeLayout_) shadowPipeline_.start(dev, shadowCs_, computeLayout_, "clouds shadow");
    VkSamplerCreateInfo s{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    s.magFilter = s.minFilter = VK_FILTER_LINEAR;
    s.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    s.addressModeU = s.addressModeV = s.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    s.maxLod = static_cast<float>(CL_NOISE_MIPS);
    if (vkCreateSampler(dev, &s, nullptr, &noiseSampler_) != VK_SUCCESS) noiseSampler_ = VK_NULL_HANDLE;
    s.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    s.addressModeU = s.addressModeV = s.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    s.maxLod = 0.0f;
    if (vkCreateSampler(dev, &s, nullptr, &coverSampler_) != VK_SUCCESS) coverSampler_ = VK_NULL_HANDLE;
    // (The cache: a level's window wraps across, its levels lie up z.)
    s.addressModeU = s.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    if (vkCreateSampler(dev, &s, nullptr, &cacheSampler_) != VK_SUCCESS) cacheSampler_ = VK_NULL_HANDLE;
    // What binding 24 of the lighting set holds with no clouds (never read).
    if (!marchLayout_ || !computeLayout_ || !noiseLayout_ || !skyVs_ || !marchFs_ || !noisePipeline_.usable() ||
        !lightPipeline_.usable() || !distPipeline_.usable() || !shadowPipeline_.usable() ||
        !shellsSortPipeline_.usable() || !shellsVs_ || !shellsFs_ || !noiseSampler_ ||
        !coverSampler_ || !cacheSampler_ || !volume_.setup(gpu, computeLayout_) ||
        !gpu.allocator.createStorageImage(1, 1, 1, kShadowFormat, shadowNone_, false, 1)) {
        LOG_ERROR("SceneClouds: failed creating the clouds' pipelines");
        return false;
    }
    return true;
}

void SceneClouds::cleanup(SceneGpu& gpu) {
    VkDevice dev = gpu.device.device();
    for (auto& s : sets_) {
        if (!s) continue;
        s->pipelines.destroy(gpu.device);
        for (AsyncComputePipeline* p : {&s->cover, &s->cache, &s->globe, &s->heaps, &s->view, &s->shells}) p->destroy(dev);
        s->destroy(dev);
    }
    sets_.clear();
    byHash_.clear();
    current_ = nullptr;
    chartValid_ = false;
    cacheBaked_ = false;
    cachePending_ = 0;
    heapBaked_ = false;
    heapPending_ = 0;
    heapKey_ = 0;
    coverHeld_ = false;
    bakingKey_ = 0;
    coverFace_ = 0;
    bakedGeneration_ = 0;
    for (SceneVkImage* i :
         {&cover_, &shadow_, &coverNone_, &shadowNone_, &gridNone_, &cacheSigma_, &cacheLight_, &cacheBand_, &distA_,
          &distB_, &cacheHolds_, &globe_, &heaps_, &view_})
        if (i->isValid()) gpu.allocator.destroyImage(*i);
    if (shells_.isValid()) gpu.allocator.destroyBuffer(shells_);
    shellBoxes_.clear();
    globeKey_ = 0;
    volume_.cleanup(gpu);
    if (noise_.image) {
        for (VkImageView v : noise_.mips)
            if (v) vkDestroyImageView(dev, v, nullptr);
        if (noise_.sampled) vkDestroyImageView(dev, noise_.sampled, nullptr);
        vkDestroyImage(dev, noise_.image, nullptr);
        if (noise_.allocId) gpu.device.context().memoryPool().free(dev, noise_.allocId);
    }
    noise_ = Noise{};
    for (AsyncComputePipeline* p : {&noisePipeline_, &lightPipeline_, &distPipeline_, &shadowPipeline_, &shellsSortPipeline_})
        p->destroy(dev);
    shellPipelines_.destroy(gpu.device);
    SceneVkShaderModule::destroy(dev, shellsSortCs_);
    SceneVkShaderModule::destroy(dev, shellsVs_);
    SceneVkShaderModule::destroy(dev, shellsFs_);
    shellsSortCs_ = shellsVs_ = shellsFs_ = VK_NULL_HANDLE;
    SceneVkShaderModule::destroy(dev, noiseCs_);
    SceneVkShaderModule::destroy(dev, lightCs_);
    SceneVkShaderModule::destroy(dev, distCs_);
    SceneVkShaderModule::destroy(dev, shadowCs_);
    lightCs_ = distCs_ = shadowCs_ = VK_NULL_HANDLE;
    marchPipelines_.destroy(gpu.device);
    SceneVkShaderModule::destroy(dev, skyVs_);
    SceneVkShaderModule::destroy(dev, marchFs_);
    SceneVkShaderModule::destroy(dev, marchVolumeFs_);
    marchFs_ = marchVolumeFs_ = VK_NULL_HANDLE;
    for (VkPipelineLayout* l : {&marchLayout_, &computeLayout_, &noiseLayout_})
        if (*l) vkDestroyPipelineLayout(dev, *l, nullptr);
    for (VkDescriptorSetLayout* l : {&setLayout_, &noiseSetLayout_})
        if (*l) vkDestroyDescriptorSetLayout(dev, *l, nullptr);
    for (VkSampler* s : {&noiseSampler_, &coverSampler_, &cacheSampler_})
        if (*s) vkDestroySampler(dev, *s, nullptr);
    noiseCs_ = skyVs_ = VK_NULL_HANDLE;
    marchLayout_ = computeLayout_ = noiseLayout_ = VK_NULL_HANDLE;
    setLayout_ = noiseSetLayout_ = VK_NULL_HANDLE;
    noiseSampler_ = coverSampler_ = cacheSampler_ = VK_NULL_HANDLE;
}

VkImageView SceneClouds::shadowView() const { return shadowsOn_ && shadow_.isValid() ? shadow_.view : shadowNone_.view; }

namespace {

// The land a deck reads the grid of, when it has processes.
LandNode* cloudLand(SceneFrame& frame, uint32_t id) {
    if (!id) return nullptr;
    SceneNode* n = frame.graph.findById(id);
    LandNode* land = n && n->type() == SceneNode::Type::Mesh ? static_cast<MeshNode*>(n)->asLand() : nullptr;
    return land && land->process().active() && land->round() ? land : nullptr;
}

}  // namespace

CloudShaders* SceneClouds::shaders(SceneFrame& frame) {
    const CloudSettings& c = frame.renderer.clouds();
    const LandNode* land = cloudLand(frame, c.land);
    const std::string grid = land ? land->gridConfig() : std::string();
    const uint64_t key = mixBits(c.sourceHash, moduleSourceHash(grid));
    auto it = byHash_.find(key);
    if (it == byHash_.end()) {
        // Compiled once a module, off the frame thread (the SPIR-V cache
        // keeps it across runs; pipelinesReady says when it is in).
        auto set = std::make_unique<CloudShaders>();
        set->start(c.source, grid);
        const int index = static_cast<int>(sets_.size());
        set->index = static_cast<uint32_t>(index);
        sets_.push_back(std::move(set));
        it = byHash_.emplace(key, index).first;
    }
    currentKey_ = key;
    if (it->second < 0) return nullptr;
    CloudShaders* set = sets_[static_cast<size_t>(it->second)].get();
    // One that does not compile is logged once and draws no clouds.
    std::string err;
    const ModuleShaders::State st = set->poll(device_->device(), err);
    if (st == ModuleShaders::State::Ready && !set->cover.started()) {
        VkDevice dev = device_->device();
        set->cover.start(dev, (*set)[CloudShaders::Cover], computeLayout_, "clouds cover");
        set->cache.start(dev, (*set)[CloudShaders::Cache], computeLayout_, "clouds cache");
        set->globe.start(dev, (*set)[CloudShaders::Globe], computeLayout_, "clouds globe");
        set->heaps.start(dev, (*set)[CloudShaders::Heaps], computeLayout_, "clouds heaps");
        set->view.start(dev, (*set)[CloudShaders::View], computeLayout_, "clouds view");
        set->shells.start(dev, (*set)[CloudShaders::Shells], computeLayout_, "clouds shells");
    }
    if (st == ModuleShaders::State::Failed || pipelinesReady(*set) == Readiness::Failed) {
        if (err.empty()) err = "its compute pipelines could not be made";
        LOG_ERROR("The clouds' module did not compile, so no clouds are drawn:\n%s", err.c_str());
        it->second = -1;
        return nullptr;
    }
    return set;
}

SceneClouds::Readiness SceneClouds::pipelinesReady(CloudShaders& set) {
    if (set.state() == ModuleShaders::State::Pending) return Readiness::Building;
    if (set.state() != ModuleShaders::State::Ready) return Readiness::Failed;
    Readiness r = Readiness::Ready;
    for (AsyncComputePipeline* p : {&set.cover, &set.cache, &set.globe, &set.heaps, &set.view, &set.shells, &noisePipeline_,
                                    &lightPipeline_, &distPipeline_, &shadowPipeline_, &shellsSortPipeline_}) {
        if (!p->get()) {
            if (!p->usable()) return Readiness::Failed;
            r = Readiness::Building;
        }
    }
    switch (volume_.readiness()) {
    case SceneCloudVolume::Readiness::Failed: return Readiness::Failed;
    case SceneCloudVolume::Readiness::Building: return Readiness::Building;
    case SceneCloudVolume::Readiness::Ready: break;
    }
    return r;
}

bool SceneClouds::ensureImages(SceneGpu& gpu) {
    if (cover_.isValid()) return true;
    if (!volume_.ensureImages(gpu)) return false;
    SceneVkAllocator& a = gpu.allocator;
    const uint32_t cacheDepth = CL_CACHE_LEVELS * CL_CACHE_BINS;
    if (!a.createStorageImage(CL_COVER_SIDE, CL_COVER_SIDE, 1, kCoverFormat, cover_, false, 6) ||
        !a.createStorageImage(CL_SHADOW_SIDE, CL_SHADOW_SIDE, 1, kShadowFormat, shadow_, false, CL_SHADOW_LEVELS) ||
        !a.createStorageImage(CL_CACHE_SIDE, CL_CACHE_SIDE, cacheDepth, kSigmaFormat, cacheSigma_, false) ||
        !a.createStorageImage(CL_CACHE_SIDE, CL_CACHE_SIDE, cacheDepth, kCacheFormat, cacheLight_, false) ||
        !a.createStorageImage(CL_CACHE_SIDE, CL_CACHE_SIDE, 1, kCacheFormat, cacheBand_, false, 3 * CL_CACHE_LEVELS) ||
        !a.createStorageImage(CL_CACHE_SIDE, CL_CACHE_SIDE, cacheDepth, kDistFormat, distA_, false) ||
        !a.createStorageImage(CL_CACHE_SIDE, CL_CACHE_SIDE, cacheDepth, kDistFormat, distB_, false) ||
        !a.createStorageImage(CL_CACHE_SIDE, CL_CACHE_SIDE, cacheDepth, kDistFormat, cacheHolds_, false) ||
        !a.createStorageImage(CL_GLOBE_SIDE, CL_GLOBE_SIDE, 1, kCacheFormat, globe_, false, 6) ||
        !a.createStorageImage(CL_HEAP_SIDE, CL_HEAP_SIDE, 1, kHeapFormat, heaps_, false, CL_HEAP_LEVELS) ||
        !a.createStorageImage(CL_VIEW_W, CL_VIEW_H, 1, kCacheFormat, view_, false, 1) ||
        !a.createStorageImage(1, 1, 1, kCoverFormat, coverNone_, false, 1) ||
        !a.createStorageImage(1, 1, 1, kGridFormat, gridNone_, false, 1) ||
        !a.createBuffer(kShellBytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT |
                        VK_BUFFER_USAGE_TRANSFER_DST_BIT, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, shells_) ||
        !createNoise(gpu)) {
        LOG_ERROR("SceneClouds: failed creating the clouds' images");
        for (SceneVkImage* i :
             {&cover_, &shadow_, &coverNone_, &gridNone_, &cacheSigma_, &cacheLight_, &cacheBand_, &distA_, &distB_,
              &cacheHolds_, &globe_, &heaps_, &view_})
            if (i->isValid()) a.destroyImage(*i);
        if (shells_.isValid()) a.destroyBuffer(shells_);
        return false;
    }
    return true;
}

bool SceneClouds::createNoise(SceneGpu& gpu) {
    if (noise_.image) return true;
    VkDevice dev = gpu.device.device();
    render::VulkanContext& ctx = gpu.device.context();
    VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    info.imageType = VK_IMAGE_TYPE_3D;
    info.extent = {CL_NOISE_SIDE, CL_NOISE_SIDE, CL_NOISE_SIDE};
    info.mipLevels = CL_NOISE_MIPS;
    info.arrayLayers = 1;
    info.format = kNoiseFormat;
    info.tiling = VK_IMAGE_TILING_OPTIMAL;
    info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    info.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    info.samples = VK_SAMPLE_COUNT_1_BIT;
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (vkCreateImage(dev, &info, nullptr, &noise_.image) != VK_SUCCESS) {
        noise_.image = VK_NULL_HANDLE;
        return false;
    }
    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(dev, noise_.image, &req);
    const uint32_t memType = ctx.findMemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    void* mapped = nullptr;
    VkDeviceSize offset = 0;
    if (memType == render::VulkanContext::kNoMemoryType ||
        !ctx.memoryPool().allocate(dev, req.size, req.alignment, memType, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                                   /*isImage=*/true, noise_.allocId, noise_.memory, offset, mapped) ||
        vkBindImageMemory(dev, noise_.image, noise_.memory, offset) != VK_SUCCESS) {
        vkDestroyImage(dev, noise_.image, nullptr);
        if (noise_.allocId) ctx.memoryPool().free(dev, noise_.allocId);
        noise_ = Noise{};
        return false;
    }
    VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    view.image = noise_.image;
    view.viewType = VK_IMAGE_VIEW_TYPE_3D;
    view.format = kNoiseFormat;
    view.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, CL_NOISE_MIPS, 0, 1};
    bool ok = vkCreateImageView(dev, &view, nullptr, &noise_.sampled) == VK_SUCCESS;
    for (uint32_t m = 0; ok && m < CL_NOISE_MIPS; ++m) {
        view.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, m, 1, 0, 1};
        ok = vkCreateImageView(dev, &view, nullptr, &noise_.mips[m]) == VK_SUCCESS;
    }
    return ok;
}

void SceneClouds::recordNoise(SceneFrame& frame) {
    VkCommandBuffer cmd = frame.cmd;
    const VkImageSubresourceRange all{VK_IMAGE_ASPECT_COLOR_BIT, 0, CL_NOISE_MIPS, 0, 1};
    render::cmdTransitionImage(cmd, noise_.image, all, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL);
    bindScenePipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, noisePipeline_.get());
    for (int m = 0; m < CL_NOISE_MIPS; ++m) {
        VkDescriptorSet set = frame.gpu.device.frameSet(noiseSetLayout_);
        SceneVkDescriptorWriter w;
        w.writeImage(0, noise_.mips[m], VK_NULL_HANDLE, VK_IMAGE_LAYOUT_GENERAL, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE);
        w.writeImage(1, noise_.mips[m > 0 ? m - 1 : 0], VK_NULL_HANDLE, VK_IMAGE_LAYOUT_GENERAL,
                     VK_DESCRIPTOR_TYPE_STORAGE_IMAGE);
        w.updateSet(device_->device(), set);
        const int32_t push[2] = {m, CL_NOISE_SIDE >> m};
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, noiseLayout_, 0, 1, &set, 0, nullptr);
        vkCmdPushConstants(cmd, noiseLayout_, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), push);
        const uint32_t groups = std::max(1, (push[1] + CL_NOISE_GROUP - 1) / CL_NOISE_GROUP);
        vkCmdDispatch(cmd, groups, groups, groups);
        memoryBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT,
                      VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT);
    }
    memoryBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT, kReaders,
                  VK_ACCESS_SHADER_READ_BIT);
    noise_.made = true;
}

void SceneClouds::beginFrame(SceneFrame& frame) {
    const CloudSettings& c = frame.renderer.clouds();
    on_ = false;
    shadowsOn_ = false;
    bake_ = false;
    shadowBlock_ = CloudShadowGpu{};
    CloudStats stats = frame.renderer.cloudStats();
    if (!c.enabled || c.sourceHash == 0) {
        frame.renderer.setCloudStats(CloudStats{});
        return;
    }
    if (!frame.gpu.atmosphere.active() || !frame.view.perspective) return;
    current_ = shaders(frame);
    // (Still compiling or building: nothing of the deck yet, said as not
    // yet compiled, the settle state's "clouds: not yet compiled".)
    const bool building = current_ && pipelinesReady(*current_) != Readiness::Ready;
    stats.compiled = current_ != nullptr && !building;
    stats.failed = current_ == nullptr;
    if (building) current_ = nullptr;
    if (!current_ || !ensureImages(frame.gpu)) {
        frame.renderer.setCloudStats(stats);
        return;
    }

    // The cover: baked again for a new generation or module, once the
    // land it reads (when it reads one) is ready, a face a frame (the six
    // at once were 43 ms of one frame's GPU on a planet). Until the first is
    // whole the deck is not drawn; a bake of a deck drawn before shows the
    // faces done and the rest as they were, and its cache whole once it is.
    coverDone_ = false;
    if (bakedGeneration_ != c.generation || bakedHash_ != currentKey_ || bakedEpoch_ != shaderSetBakeEpoch()) {
        bakeGrid_ = gridNone_.view;
        if (c.land) {
            SceneNode* n = frame.graph.findById(c.land);
            LandNode* land = n && n->type() == SceneNode::Type::Mesh ? static_cast<MeshNode*>(n)->asLand() : nullptr;
            if (land && land->process().active()) {
                const VkImageView grid = frame.gpu.meshes.landGrid(land->id());
                if (!land->ready() || !grid) {
                    stats.covered = false;
                    frame.renderer.setCloudStats(stats);
                    return;
                }
                if (land->round()) bakeGrid_ = grid;
            }
        }
        const uint64_t key = mixBits(mixBits(currentKey_, c.generation), shaderSetBakeEpoch()) | 1u;
        if (key != bakingKey_) {
            bakingKey_ = key;
            coverFace_ = 0;
        }
        bake_ = true;
        coverDone_ = coverFace_ == 5;
    }
    const bool held = !bake_ || coverDone_ || coverHeld_;
    stats.covered = held;
    stats.generation = c.generation;
    frame.renderer.setCloudStats(stats);
    on_ = true;

    // The block: the place frame's turn into the body's, the shell, the step.
    const SceneAtmosphere& atm = frame.gpu.atmosphere;
    const float R = atm.planetRadius();
    const double* b = frame.graph.place().basis();
    for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 3; ++j) block_.toBody[i][j] = frame.graph.hasBody() ? static_cast<float>(b[i * 3 + j]) : (i == j ? 1.0f : 0.0f);
        block_.toBody[i][3] = 0.0f;
    }
    const float bottom = std::max(c.bottom, 0.0f), top = std::max(c.top, bottom + 1.0f);
    block_.shell[0] = R + bottom;
    block_.shell[1] = R + top;
    block_.shell[2] = R;
    block_.shell[3] = frame.renderer.windTime();
    // A pixel's angle (its height over the view's), the shortest step and a
    // step's share of its distance.
    const float p11 = std::fabs(frame.view.proj.at(1, 1));
    block_.params[0] = std::clamp(c.albedo, 0.0f, 1.0f);
    block_.params[1] = p11 > 0.0f && frame.view.height ? 2.0f / (p11 * static_cast<float>(frame.view.height)) : 1e-3f;
    block_.params[2] = std::max((top - bottom) / 32.0f, 20.0f);
    block_.params[3] = 0.15f;
    block_.forms[0] = c.extinction;
    block_.forms[1] = c.billowScale;
    block_.forms[2] = c.detailScale;
    block_.forms[3] = c.edge;
    for (int i = 0; i < 3; ++i) block_.wind[i] = c.wind[i];
    block_.wind[3] = c.heapCell;   // (the heaps' lattice cell: sky/cloud_heaps.glsl)
    const size_t vecs = std::min(c.data.size() / 4, static_cast<size_t>(CL_MAX_DATA));
    std::memset(block_.data, 0, sizeof(block_.data));
    std::memcpy(block_.data, c.data.data(), vecs * 4 * sizeof(float));

    // The shadow's levels: about the body's point under the eye, on the
    // plane tangent there.
    float eye[4];
    atm.eyeAbout(frame.world.eye, eye);
    const float el = std::sqrt(eye[0] * eye[0] + eye[1] * eye[1] + eye[2] * eye[2]);
    eyeRadius_ = el;
    const float n[3] = {eye[0] / el, eye[1] / el, eye[2] / el};
    float a[3] = {1.0f, 0.0f, 0.0f};
    if (std::fabs(n[0]) > 0.9f) a[0] = 0.0f, a[2] = 1.0f;
    const float an = a[0] * n[0] + a[1] * n[1] + a[2] * n[2];
    float e1[3] = {a[0] - an * n[0], a[1] - an * n[1], a[2] - an * n[2]};
    const float l1 = std::sqrt(e1[0] * e1[0] + e1[1] * e1[1] + e1[2] * e1[2]);
    for (float& v : e1) v /= l1;
    const float e2[3] = {n[1] * e1[2] - n[2] * e1[1], n[2] * e1[0] - n[0] * e1[2], n[0] * e1[1] - n[1] * e1[0]};
    for (int k = 0; k < 3; ++k) shadowNadir_[k] = n[k];
    placeShadow(n, e1, e2, R, c);
    shadowsOn_ = c.shadows;
    shadowBlock_.params[0] = shadowsOn_ ? 1.0f : 0.0f;
    shadowBlock_.params[1] = R;
    shadowBlock_.params[2] = CL_SHADOW_TEXEL0;
    shadowBlock_.params[3] = CL_SHADOW_GROWTH;
    shadowBlock_.deck[0] = block_.shell[0];
    shadowBlock_.deck[1] = block_.shell[1];
    if (!held) {
        // (The block filled for the cover's face; nothing else of the deck.)
        on_ = shadowsOn_ = false;
        shadowBlock_ = CloudShadowGpu{};
        return;
    }
    placeCache(frame);

    // The volume about the eye: the eye in the body's frame as the march
    // takes it (the atmosphere's origin, turned).
    SceneCloudVolume::Frame vf{};
    for (int i = 0; i < 3; ++i) {
        vf.eye[i] = 0.0;
        for (int j = 0; j < 3; ++j) vf.eye[i] += static_cast<double>(block_.toBody[i][j]) * eye[j];
        vf.wind[i] = block_.wind[i];
    }
    vf.windTime = block_.shell[3];
    vf.Rb = block_.shell[0];
    vf.Rt = block_.shell[1];
    vf.pixel = block_.params[1];
    vf.key = cacheKey_;
    vf.jump = std::fabs(block_.shell[3] - volumeTime_) > 0.25f;
    volumeTime_ = block_.shell[3];
    CloudStats vstats = frame.renderer.cloudStats();
    vf.sourceSettled = vstats.cachePending == 0;
    volume_.place(vf, block_.volume, vstats);
    frame.renderer.setCloudStats(vstats);

    // The view's columns: made while the eye is over the deck and far
    // enough that the layer may draw some of it (a pixel CL_LAYER_FROM of
    // the deck's thickness at the horizon or nearer), a texel of them every
    // `scale` pixels each way.
    block_.viewSize[2] = 0.0f;
    const float horizon = el > block_.shell[1] ? std::sqrt(el * el - block_.shell[1] * block_.shell[1]) : 0.0f;
    if (horizon * block_.params[1] >= 0.02f * (top - bottom) && frame.view.width && frame.view.height) {
        const uint32_t scale = std::max({2u, (frame.view.width + CL_VIEW_W - 1) / CL_VIEW_W,
                                         (frame.view.height + CL_VIEW_H - 1) / CL_VIEW_H});
        block_.viewSize[0] = static_cast<float>((frame.view.width + scale - 1) / scale);
        block_.viewSize[1] = static_cast<float>((frame.view.height + scale - 1) / scale);
        block_.viewSize[2] = 1.0f;
        block_.viewSize[3] = static_cast<float>(scale);
        for (int i = 0; i < 3; ++i) {
            for (int j = 0; j < 3; ++j) block_.viewToWorld[i][j] = frame.view.invRelView.at(i, j);
            block_.viewToWorld[i][3] = 0.0f;
            block_.viewEye[i] = eye[i];
        }
        for (int i = 0; i < 4; ++i)
            for (int j = 0; j < 4; ++j) block_.viewInvProj[i][j] = frame.view.invProj.at(i, j);
    }
    placeShells(frame, eye);
}

// The shadow's levels baked every frame; the coarser ones a level a frame
// in turn, and any whose nadir the eye's has left by more than
// 1 / kShadowMoved of its half-width (8 texels), so the land a level holds
// is about the eye however fast it goes. (The clouds drift a texel of the
// third level in some 20 s: a coarse level a few frames old shows them
// where they are.)
constexpr int kShadowEvery = 2;
constexpr double kShadowMoved = 32.0;
static_assert(CL_SHADOW_LEVELS <= 12 && CL_SHADOW_LEVELS > kShadowEvery);

void SceneClouds::placeShadow(const float n[3], const float e1[3], const float e2[3], float R,
                              const CloudSettings& c) {
    // (The eye's nadir and axes, air space, into the body's frame.)
    const auto body = [&](const float* a, double* out) {
        for (int i = 0; i < 3; ++i)
            out[i] = static_cast<double>(block_.toBody[i][0]) * a[0] + block_.toBody[i][1] * a[1] + block_.toBody[i][2] * a[2];
    };
    double now[3][3];
    body(n, now[0]);
    body(e1, now[1]);
    body(e2, now[2]);
    // All again for another deck, a jump in the wind's time, or the first.
    const uint64_t key = mixBits(mixBits(currentKey_, c.generation), shaderSetBakeEpoch());
    const bool every = !shadowValid_ || key != shadowKey_ || std::fabs(block_.shell[3] - shadowTime_) > 0.25f;
    shadowValid_ = true;
    shadowKey_ = key;
    shadowTime_ = block_.shell[3];
    const int turn = kShadowEvery + static_cast<int>(shadowTurn_++ % (CL_SHADOW_LEVELS - kShadowEvery));
    std::memset(block_.shadowBake, 0, sizeof(block_.shadowBake));
    shadowBakes_ = 0;
    double texel = CL_SHADOW_TEXEL0;
    for (int l = 0; l < CL_SHADOW_LEVELS; ++l, texel *= CL_SHADOW_GROWTH) {
        double (&lv)[3][3] = shadowLevel_[l];
        const double d = lv[0][0] * now[0][0] + lv[0][1] * now[0][1] + lv[0][2] * now[0][2];
        const double moved = std::acos(std::clamp(d, -1.0, 1.0)) * R;
        if (every || l < kShadowEvery || l == turn || moved > 0.5 * CL_SHADOW_SIDE * texel / kShadowMoved) {
            std::memcpy(lv, now, sizeof(now));
            block_.shadowBake[shadowBakes_ / 4][shadowBakes_ % 4] = static_cast<float>(l);
            ++shadowBakes_;
        }
        // (Back into air space: the transpose of the turn into the body's.)
        float air[3][3];
        for (int a = 0; a < 3; ++a)
            for (int j = 0; j < 3; ++j)
                air[a][j] = static_cast<float>(block_.toBody[0][j] * lv[a][0] + block_.toBody[1][j] * lv[a][1] +
                                               block_.toBody[2][j] * lv[a][2]);
        for (int j = 0; j < 3; ++j) {
            block_.shadowN[l][j] = shadowBlock_.n[l][j] = air[0][j];
            block_.shadowE1[l][j] = shadowBlock_.e1[l][j] = air[1][j];
            block_.shadowE2[l][j] = shadowBlock_.e2[l][j] = air[2][j];
        }
        block_.shadowN[l][3] = static_cast<float>(texel);
    }
}

void SceneClouds::recordVolume(SceneFrame& frame) {
    if (!on_ || !current_) return;
    VkDescriptorSet set = writeSet(frame, false, false);
    volume_.record(frame.cmd, set, computeLayout_);
}

void SceneClouds::placeCache(SceneFrame& frame) {
    const CloudSettings& c = frame.renderer.clouds();
    const double Rb = block_.shell[0];
    // The nadir in the body's frame: on a round body the eye's direction
    // from the centre, the eye taken onto the body in double (the place's
    // origin and axes as they are this frame, so a re-anchor leaves it
    // where it was); otherwise the shadow's (the air's) up under the eye.
    double nb[3];
    const Place& place = frame.graph.place();
    if (frame.graph.hasBody() && !place.body().flat())
        place.toBody(frame.world.eye, nb);
    else
        for (int i = 0; i < 3; ++i)
            nb[i] = block_.toBody[i][0] * shadowNadir_[0] + block_.toBody[i][1] * shadowNadir_[1] +
                    block_.toBody[i][2] * shadowNadir_[2];
    const double nl = std::sqrt(nb[0] * nb[0] + nb[1] * nb[1] + nb[2] * nb[2]);
    for (double& v : nb) v /= nl;
    const auto dot = [](const double* a, const double* b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; };

    // (A shader set switched to with a rebake asked bakes everything again:
    // scene_shader_sets.h.)
    const uint64_t key = mixBits(mixBits(currentKey_, c.generation), shaderSetBakeEpoch());
    bool all = !chartValid_ || key != cacheKey_ || coverDone_;
    // (A frame's jump in the wind's time: a deck that moved, not drifted.)
    const float windStep = block_.shell[3] - cacheTime_;
    all = all || std::fabs(windStep) > 0.25f;
    cacheTime_ = block_.shell[3];
    // The chart: about the nadir, again when the nadir is far from its
    // centre (the chart's cells stretch away from it).
    if (!chartValid_ || std::acos(std::clamp(dot(nb, chartC_), -1.0, 1.0)) * Rb > 1.0e6) {
        for (int i = 0; i < 3; ++i) chartC_[i] = nb[i];
        double a[3] = {1.0, 0.0, 0.0};
        if (std::fabs(nb[0]) > 0.9) a[0] = 0.0, a[2] = 1.0;
        const double an = dot(a, nb);
        for (int i = 0; i < 3; ++i) chartE1_[i] = a[i] - an * nb[i];
        const double l1 = std::sqrt(dot(chartE1_, chartE1_));
        for (double& v : chartE1_) v /= l1;
        chartE2_[0] = nb[1] * chartE1_[2] - nb[2] * chartE1_[1];
        chartE2_[1] = nb[2] * chartE1_[0] - nb[0] * chartE1_[2];
        chartE2_[2] = nb[0] * chartE1_[1] - nb[1] * chartE1_[0];
        chartValid_ = true;
        all = true;
        cacheBaked_ = false;   // (every cell of every level is elsewhere)
        ++chartSerial_;
    }
    cacheKey_ = key;
    // The nadir on the chart.
    double x, y;
    chartAt(nb, x, y);
    for (int i = 0; i < 3; ++i) {
        block_.cacheC[i] = static_cast<float>(chartC_[i]);
        block_.cacheE1[i] = static_cast<float>(chartE1_[i]);
        block_.cacheE2[i] = static_cast<float>(chartE2_[i]);
    }
    block_.cacheC[3] = static_cast<float>(Rb);
    block_.cacheE1[3] = (block_.shell[1] - block_.shell[0]) / static_cast<float>(CL_CACHE_BINS);
    block_.cacheE2[3] = 0.0f;
    block_.cacheNadir[0] = static_cast<float>(x);
    block_.cacheNadir[1] = static_cast<float>(y);
    block_.cacheNadir[2] = block_.cacheNadir[3] = 0.0f;

    // The levels' windows: about the cell that holds the nadir. One that
    // moved bakes the cells it took in (and those about them whose band
    // and distance to cloud they change); every level's rows a share a
    // frame.
    bakes_.clear();
    const bool whole = all && !cacheBaked_;
    for (int k = 0; k < CL_CACHE_LEVELS; ++k) {
        // (A window moves once the nadir is a quarter cell past the cell it
        // is about, so a nadir on a cell's edge does not move it back and
        // forth; a lookup CL_CACHE_MARGIN cells from it, filtered, stays in.)
        const double cell = CL_CACHE_TEXEL0 * std::ldexp(1.0, k);
        const auto follow = [&](double u, long long c) {
            const long long f = static_cast<long long>(std::floor(u));
            const bool near = u >= static_cast<double>(c) - 0.25 && u < static_cast<double>(c) + 1.25;
            return whole || !cacheBaked_ || !near ? f : c;
        };
        const long long cx = follow(x / cell, levelCentre_[k][0]);
        const long long cy = follow(y / cell, levelCentre_[k][1]);
        if (!whole && cacheBaked_) {
            moveLevel(k, 0, levelCentre_[k][0], cx);
            moveLevel(k, 1, levelCentre_[k][1], cy);
        }
        levelCentre_[k][0] = cx;
        levelCentre_[k][1] = cy;
        block_.cacheLevel[k][0] = static_cast<float>(cx);
        block_.cacheLevel[k][1] = static_cast<float>(cy);
        block_.cacheLevel[k][2] = block_.cacheLevel[k][3] = 0.0f;
    }
    if (whole) {
        // (The scene's first: nothing to read meanwhile.)
        bakes_.assign(1, CacheBake::level(0, CL_CACHE_LEVELS));
        cachePending_ = 0;
        cacheBaked_ = true;
    } else {
        if (all) cachePending_ = (1u << CL_CACHE_LEVELS) - 1u;
        // A rebake of every level, a level a frame and the coarsest first
        // (a level's light reads its own and the coarser's), each whole in
        // its frame; the march reads what the others held meanwhile.
        if (cachePending_) {
            int k = CL_CACHE_LEVELS - 1;
            while (!(cachePending_ & (1u << k))) --k;
            cachePending_ &= ~(1u << k);
            bakes_.push_back(CacheBake::level(k, 1));
        }
        // The wind's drift: a slice of a level a unit, coarsest first, as
        // many a frame as the wind's time since owes (every row of every
        // level each CL_CACHE_REFRESH seconds of it), so a frame's share
        // falls as the frame rate rises and none is made while the wind's
        // time stands. (A slice of every level a frame cost 0.3 ms a frame
        // at the ground at 30 frames a second and the same at 360, twelve
        // times the drift's need.) A frame's units of one slice, levels in
        // a run, are one bake.
        constexpr int kUnits = CL_CACHE_SLICES * CL_CACHE_LEVELS;
        cacheDebt_ = std::min(cacheDebt_ + std::max(windStep, 0.0f) / CL_CACHE_REFRESH * kUnits,
                              static_cast<double>(kUnits));
        for (; cacheDebt_ >= 1.0; cacheDebt_ -= 1.0) {
            const uint32_t u = cacheUnit_++ % kUnits;
            const int k = CL_CACHE_LEVELS - 1 - static_cast<int>(u % CL_CACHE_LEVELS);
            const int y0 = static_cast<int>(u / CL_CACHE_LEVELS);
            if (!bakes_.empty() && bakes_.back().yStep == CL_CACHE_SLICES && bakes_.back().y0 == y0 &&
                bakes_.back().first == k + 1) {
                --bakes_.back().first;
                ++bakes_.back().levels;
            } else {
                CacheBake slice = CacheBake::level(k, 1);
                slice.y0 = y0;
                slice.yStep = CL_CACHE_SLICES;
                slice.ny = CL_CACHE_SIDE / CL_CACHE_SLICES;
                bakes_.push_back(slice);
            }
            unitRun_ = std::min(unitRun_ + 1, kUnits);
        }
        // (Nothing drifts while the wind's time stands: no slice is owed.)
        if (windStep <= 0.0f) unitRun_ = kUnits;
    }
    // What is still to come of it for this view (the settle state): the
    // levels to bake again, and the slices since the last whole change.
    if (all) unitRun_ = 0;
    CloudStats stats = frame.renderer.cloudStats();
    stats.cachePending = std::popcount(cachePending_) +
                         (CL_CACHE_SLICES * CL_CACHE_LEVELS - unitRun_ + CL_CACHE_LEVELS - 1) / CL_CACHE_LEVELS;
    frame.renderer.setCloudStats(stats);
    placeHeaps(frame, nb);
}

void SceneClouds::chartAt(const double u[3], double& x, double& y) const {
    const auto dot = [](const double* a, const double* b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; };
    const double vx = dot(u, chartE1_), vy = dot(u, chartE2_);
    const double sv = std::sqrt(vx * vx + vy * vy);
    const double th = std::atan2(sv, dot(u, chartC_));
    const double Rb = block_.shell[0];
    const double scale = sv > 1e-12 ? th * Rb / sv : Rb;
    x = vx * scale;
    y = vy * scale;
}

void SceneClouds::placeHeaps(SceneFrame& frame, const double nb[3]) {
    const CloudSettings& c = frame.renderer.clouds();
    heapBakes_.clear();
    std::memset(block_.heapNadir, 0, sizeof(block_.heapNadir));
    std::memset(block_.heapLevel, 0, sizeof(block_.heapLevel));
    if (c.heapCell <= 0.0f) {
        heapBaked_ = false;
        heapPending_ = 0;
        return;
    }
    // (The deck the cache holds and the lattice's cell; a chart about a new
    // centre puts every texel elsewhere.)
    uint32_t cellBits;
    std::memcpy(&cellBits, &c.heapCell, sizeof(cellBits));
    const uint64_t key = mixBits(cacheKey_, cellBits) | 1u;
    const bool all = key != heapKey_ || coverDone_ || heapChart_ != chartSerial_;
    if (heapChart_ != chartSerial_) heapBaked_ = false;
    heapKey_ = key;
    heapChart_ = chartSerial_;
    // The drifted nadir (the heaps drift with the wind: cloudDrift), on the chart.
    const double Rb = block_.shell[0], t = block_.shell[3];
    double pd[3];
    for (int i = 0; i < 3; ++i) pd[i] = nb[i] * Rb - static_cast<double>(block_.wind[i]) * t;
    const double pl = std::sqrt(pd[0] * pd[0] + pd[1] * pd[1] + pd[2] * pd[2]);
    for (double& v : pd) v /= pl;
    double x, y;
    chartAt(pd, x, y);
    const bool whole = all && !heapBaked_;
    for (int k = 0; k < CL_HEAP_LEVELS; ++k) {
        // (As the cache's: a window moves once the nadir is a quarter texel
        // past the texel it is about.)
        const double texel = CL_HEAP_TEXEL0 * std::ldexp(1.0, k);
        const auto follow = [&](double u, long long ctr) {
            const long long f = static_cast<long long>(std::floor(u));
            const bool near = u >= static_cast<double>(ctr) - 0.25 && u < static_cast<double>(ctr) + 1.25;
            return whole || !heapBaked_ || !near ? f : ctr;
        };
        const long long cx = follow(x / texel, heapCentre_[k][0]);
        const long long cy = follow(y / texel, heapCentre_[k][1]);
        if (!whole && heapBaked_) {
            // The texels a window takes in, a strip along each axis it moved.
            constexpr int S = CL_HEAP_SIDE, H = CL_HEAP_SIDE / 2;
            for (int axis = 0; axis < 2; ++axis) {
                const long long from = heapCentre_[k][axis], to = axis == 0 ? cx : cy;
                const long long d = to - from;
                if (d == 0) continue;
                HeapBake b = HeapBake::level(k, 1);
                if (std::llabs(d) < S) {
                    const int s0 = static_cast<int>((d > 0 ? from + H : to - H) & (S - 1));
                    const int n = static_cast<int>(std::llabs(d));
                    if (axis == 0) b.x0 = s0, b.nx = n;
                    else b.y0 = s0, b.ny = n;
                }
                heapBakes_.push_back(b);
            }
        }
        heapCentre_[k][0] = cx;
        heapCentre_[k][1] = cy;
        block_.heapLevel[k][0] = static_cast<float>(cx);
        block_.heapLevel[k][1] = static_cast<float>(cy);
    }
    if (whole) {
        heapBakes_.assign(1, HeapBake::level(0, CL_HEAP_LEVELS));
        heapPending_ = 0;
        heapBaked_ = true;
    } else {
        if (all) heapPending_ = (1u << CL_HEAP_LEVELS) - 1u;
        // A rebake of every level, a level a frame and the coarsest first;
        // the march reads what the others held meanwhile.
        if (heapPending_) {
            int k = CL_HEAP_LEVELS - 1;
            while (!(heapPending_ & (1u << k))) --k;
            heapPending_ &= ~(1u << k);
            heapBakes_.push_back(HeapBake::level(k, 1));
        }
    }
    block_.heapNadir[0] = static_cast<float>(x);
    block_.heapNadir[1] = static_cast<float>(y);
    block_.heapNadir[2] = 1.0f;
    CloudStats stats = frame.renderer.cloudStats();
    stats.cachePending += std::popcount(heapPending_);
    frame.renderer.setCloudStats(stats);
}

void SceneClouds::recordHeaps(SceneFrame& frame) {
    if (heapBakes_.empty() || !current_->heaps.get()) return;
    VkDescriptorSet set = writeSet(frame, false, false);
    if (!set) return;
    VkCommandBuffer cmd = frame.cmd;
    // What earlier frames read of the map is done with; the cover baked.
    memoryBarrier(cmd, kReaders, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT,
                  VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
    bindScenePipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, current_->heaps.get());
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, computeLayout_, 0, 1, &set, 0, nullptr);
    for (const HeapBake& b : heapBakes_) {
        const int32_t push[8] = {b.first, b.y0, b.ny, 0, b.x0, b.nx, 0, 0};
        vkCmdPushConstants(cmd, computeLayout_, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), push);
        vkCmdDispatch(cmd, static_cast<uint32_t>((b.nx + CL_HEAP_GROUP - 1) / CL_HEAP_GROUP),
                      static_cast<uint32_t>((b.ny + CL_HEAP_GROUP - 1) / CL_HEAP_GROUP), static_cast<uint32_t>(b.levels));
    }
    memoryBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT, kReaders,
                  VK_ACCESS_SHADER_READ_BIT);
    if (frame.timer) frame.timer->mark(cmd, "clouds:heaps");
}

void SceneClouds::moveLevel(int k, int axis, long long from, long long to) {
    const long long d = to - from;
    if (d == 0) return;
    constexpr int S = CL_CACHE_SIDE, H = CL_CACHE_SIDE / 2, R = CL_CACHE_REACH;
    if (std::llabs(d) >= S - 2 * (R + 2)) {
        // (A jump past most of the window: little of it is kept.)
        bakes_.push_back(CacheBake::level(k, 1));
        return;
    }
    // Cells `g0` .. `g0 + n - 1` along the axis, from pass `pass0` on.
    const auto band = [&](long long g0, int n, int pass0) {
        CacheBake b = CacheBake::level(k, 1);
        b.pass0 = pass0;
        const int s0 = static_cast<int>(g0 & (S - 1));
        if (axis == 0) b.x0 = s0, b.nx = n;
        else b.y0 = s0, b.ny = n;
        bakes_.push_back(b);
    };
    // The cells taken in, whole, with the old edge's (its band reads them);
    // the distance to cloud out to the reach from them; and at the edge
    // given up, the cell whose band reads across the window's wrap to the
    // cells taken in, and the distances out to the reach that counted on
    // what was given up.
    const int n = static_cast<int>(std::llabs(d));
    if (d > 0) {
        band(from + H - 1, n + 1, 0);
        band(from + H - 1 - R, R, 2);
        band(to - H, 1, 1);
        band(to - H + 1, R, 2);
    } else {
        band(to - H, n + 1, 0);
        band(from - H + 1, R, 2);
        band(to + H - 1, 1, 1);
        band(to + H - 1 - R, R, 2);
    }
}

VkDescriptorSet SceneClouds::writeSet(SceneFrame& frame, bool march, bool bake) {
    const VkDescriptorBufferInfo ubo = frame.gpu.device.frameUniform(&block_, sizeof(block_));
    VkDescriptorSet set = frame.gpu.device.frameSet(setLayout_);
    if (set == VK_NULL_HANDLE) return set;
    const SceneTargets& t = frame.gpu.targets;
    SceneVkDescriptorWriter w;
    w.writeBuffer(CL_B_UBO, ubo.buffer, ubo.range, ubo.offset);
    w.writeImage(CL_B_COVER, bake ? coverNone_.view : cover_.view, coverSampler_, VK_IMAGE_LAYOUT_GENERAL);
    w.writeImage(CL_B_NOISE, noise_.sampled, noiseSampler_, VK_IMAGE_LAYOUT_GENERAL);
    if (march)
        w.writeImage(CL_B_DEPTH, t.depthSnapshot.view, t.depthSnapshot.sampler);
    else
        w.writeImage(CL_B_DEPTH, frame.gpu.defaults.white.view, frame.gpu.defaults.sampler);
    w.writeImage(CL_B_COVER_OUT, bake ? cover_.view : coverNone_.view, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_GENERAL,
                 VK_DESCRIPTOR_TYPE_STORAGE_IMAGE);
    w.writeImage(CL_B_SHADOW_OUT, !bake && !march ? shadow_.view : shadowNone_.view, VK_NULL_HANDLE,
                 VK_IMAGE_LAYOUT_GENERAL, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE);
    w.writeImage(CL_B_LAND_GRID, bake ? bakeGrid_ : gridNone_.view, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_GENERAL,
                 VK_DESCRIPTOR_TYPE_STORAGE_IMAGE);
    w.writeImage(CL_B_CACHE, cacheLight_.view, cacheSampler_, VK_IMAGE_LAYOUT_GENERAL);
    w.writeImage(CL_B_BAND, cacheBand_.view, cacheSampler_, VK_IMAGE_LAYOUT_GENERAL);
    w.writeImage(CL_B_BAND_OUT, cacheBand_.view, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_GENERAL,
                 VK_DESCRIPTOR_TYPE_STORAGE_IMAGE);
    w.writeImage(CL_B_DIST_A, distA_.view, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_GENERAL, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE);
    w.writeImage(CL_B_DIST_B, distB_.view, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_GENERAL, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE);
    w.writeImage(CL_B_HOLDS, cacheHolds_.view, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_GENERAL,
                 VK_DESCRIPTOR_TYPE_STORAGE_IMAGE);
    w.writeImage(CL_B_CACHE_OUT, cacheLight_.view, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_GENERAL,
                 VK_DESCRIPTOR_TYPE_STORAGE_IMAGE);
    w.writeImage(CL_B_SIGMA, cacheSigma_.view, cacheSampler_, VK_IMAGE_LAYOUT_GENERAL);
    w.writeImage(CL_B_SIGMA_OUT, cacheSigma_.view, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_GENERAL,
                 VK_DESCRIPTOR_TYPE_STORAGE_IMAGE);
    w.writeImage(CL_B_GLOBE, globe_.view, coverSampler_, VK_IMAGE_LAYOUT_GENERAL);
    w.writeImage(CL_B_GLOBE_OUT, globe_.view, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_GENERAL,
                 VK_DESCRIPTOR_TYPE_STORAGE_IMAGE);
    w.writeImage(CL_B_VIEW, view_.view, coverSampler_, VK_IMAGE_LAYOUT_GENERAL);
    w.writeImage(CL_B_VIEW_OUT, view_.view, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_GENERAL,
                 VK_DESCRIPTOR_TYPE_STORAGE_IMAGE);
    w.writeImage(CL_B_HEAPS, heaps_.view, cacheSampler_, VK_IMAGE_LAYOUT_GENERAL);
    w.writeImage(CL_B_HEAPS_OUT, heaps_.view, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_GENERAL,
                 VK_DESCRIPTOR_TYPE_STORAGE_IMAGE);
    w.writeBuffer(CL_B_SHELLS, shells_.buffer, kShellBytes, 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
    volume_.write(w);
    w.updateSet(device_->device(), set);
    return set;
}

void SceneClouds::recordPrepare(SceneFrame& frame) {
    if (!preparing() || !current_) return;
    VkCommandBuffer cmd = frame.cmd;
    const AtmosphereGpu& atm = frame.lighting.atm;
    for (int k = 0; k < 3; ++k) block_.sun[k] = atm.sunDir[k];
    block_.sun[3] = atm.sunIlluminance[3];
    // A jump in the sun (the body's frame, a quarter of a degree): every
    // shadow level again, each where it lies.
    {
        float sun[3];
        for (int i = 0; i < 3; ++i)
            sun[i] = block_.toBody[i][0] * block_.sun[0] + block_.toBody[i][1] * block_.sun[1] +
                     block_.toBody[i][2] * block_.sun[2];
        if (sun[0] * shadowSun_[0] + sun[1] * shadowSun_[1] + sun[2] * shadowSun_[2] < 0.99999f) {
            for (int l = 0; l < CL_SHADOW_LEVELS; ++l) block_.shadowBake[l / 4][l % 4] = static_cast<float>(l);
            shadowBakes_ = CL_SHADOW_LEVELS;
        }
        std::memcpy(shadowSun_, sun, sizeof(sun));
    }
    if (!noise_.made) recordNoise(frame);

    if (bake_) {
        VkDescriptorSet set = writeSet(frame, false, true);
        if (!set) return;
        // The land's grid its processes wrote, and the cover's last readers.
        memoryBarrier(cmd, kReaders | VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_SHADER_WRITE_BIT |
                      VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                      VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
        bindScenePipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, current_->cover.get());
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, computeLayout_, 0, 1, &set, 0, nullptr);
        const int32_t push[8] = {coverFace_, 0, 0, 0, 0, 0, 0, 0};
        vkCmdPushConstants(cmd, computeLayout_, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), push);
        const uint32_t groups = (CL_COVER_SIDE + CL_COVER_GROUP - 1) / CL_COVER_GROUP;
        vkCmdDispatch(cmd, groups, groups, 1);
        memoryBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT, kReaders,
                      VK_ACCESS_SHADER_READ_BIT);
        if (frame.timer) frame.timer->mark(cmd, "clouds:cover");
        if (++coverFace_ == 6) {
            bakedGeneration_ = frame.renderer.clouds().generation;
            bakedHash_ = currentKey_;
            bakedEpoch_ = shaderSetBakeEpoch();
            bakingKey_ = 0;
            coverFace_ = 0;
            coverHeld_ = true;
        }
    }
    if (!on_) return;

    recordHeaps(frame);
    recordCache(frame);
    recordGlobe(frame);
    recordView(frame);
    recordShells(frame);

    if (shadowsOn_ && shadowBakes_ > 0) {
        VkDescriptorSet set = writeSet(frame, false, false);
        if (!set) return;
        // What earlier frames read of the levels is done with.
        memoryBarrier(cmd, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT,
                      VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT);
        bindScenePipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, shadowPipeline_.get());
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, computeLayout_, 0, 1, &set, 0, nullptr);
        const uint32_t groups = (CL_SHADOW_SIDE + CL_SHADOW_GROUP - 1) / CL_SHADOW_GROUP;
        vkCmdDispatch(cmd, groups, groups, static_cast<uint32_t>(shadowBakes_));
        memoryBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT,
                      VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT);
    }
}

void SceneClouds::recordCache(SceneFrame& frame) {
    VkCommandBuffer cmd = frame.cmd;
    // A frame's jump in the sun (the body's frame): every level again.
    float sun[3];
    for (int i = 0; i < 3; ++i)
        sun[i] = block_.toBody[i][0] * block_.sun[0] + block_.toBody[i][1] * block_.sun[1] +
                 block_.toBody[i][2] * block_.sun[2];
    const float turned = sun[0] * cacheSun_[0] + sun[1] * cacheSun_[1] + sun[2] * cacheSun_[2];
    const bool sunKnown = cacheSun_[0] != 0.0f || cacheSun_[1] != 0.0f || cacheSun_[2] != 0.0f;
    if (sunKnown && turned < 0.99996f) cachePending_ = (1u << CL_CACHE_LEVELS) - 1u;   // (half a degree)
    std::memcpy(cacheSun_, sun, sizeof(sun));
    if (bakes_.empty()) return;
    VkDescriptorSet set = writeSet(frame, false, false);
    if (!set) return;
    // What earlier frames read of the cache is done with; the cover baked.
    memoryBarrier(cmd, kReaders, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT,
                  VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
    // The extinction, its light, then its distance to cloud an axis a pass.
    for (int pass = 0; pass < 5; ++pass) {
        bindScenePipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                          pass == 0 ? current_->cache.get() : pass == 1 ? lightPipeline_.get() : distPipeline_.get());
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, computeLayout_, 0, 1, &set, 0, nullptr);
        for (const CacheBake& b : bakes_) {
            if (b.pass0 > pass) continue;
            const int32_t push[8] = {b.first, b.y0, b.yStep, std::max(pass - 2, 0), b.x0, b.nx, b.ny, 0};
            vkCmdPushConstants(cmd, computeLayout_, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), push);
            // (The extinction's and the light's groups are a row's columns
            // with their whole columns of bins; the distance's a square.)
            const uint32_t gx = static_cast<uint32_t>((b.nx + CL_CACHE_GROUP - 1) / CL_CACHE_GROUP);
            if (pass < 2)
                vkCmdDispatch(cmd, gx, static_cast<uint32_t>(b.ny), static_cast<uint32_t>(b.levels));
            else
                vkCmdDispatch(cmd, gx, static_cast<uint32_t>((b.ny + CL_CACHE_GROUP - 1) / CL_CACHE_GROUP),
                              static_cast<uint32_t>(b.levels * CL_CACHE_BINS));
        }
        memoryBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT,
                      pass < 4 ? VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT : kReaders, VK_ACCESS_SHADER_READ_BIT);
    }
}

void SceneClouds::recordGlobe(SceneFrame& frame) {
    // (Read only by the layer the march draws from over the deck.)
    if (eyeRadius_ <= block_.shell[1]) return;
    const uint64_t key = mixBits(mixBits(currentKey_, frame.renderer.clouds().generation), shaderSetBakeEpoch()) | 1u;
    const bool all = key != globeKey_ || std::fabs(block_.shell[3] - globeTime_) > 0.25f;
    globeKey_ = key;
    globeTime_ = block_.shell[3];
    const int y0 = all ? 0 : static_cast<int>(globeFrame_++ % CL_GLOBE_SLICES);
    const int yStep = all ? 1 : CL_GLOBE_SLICES;
    VkDescriptorSet set = writeSet(frame, false, false);
    if (!set) return;
    VkCommandBuffer cmd = frame.cmd;
    // What earlier frames read of it is done with; the cover baked.
    memoryBarrier(cmd, kReaders, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT,
                  VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
    bindScenePipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, current_->globe.get());
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, computeLayout_, 0, 1, &set, 0, nullptr);
    const int32_t push[4] = {0, y0, yStep, 0};
    vkCmdPushConstants(cmd, computeLayout_, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), push);
    const uint32_t rows = static_cast<uint32_t>((CL_GLOBE_SIDE - y0 + yStep - 1) / yStep);
    vkCmdDispatch(cmd, CL_GLOBE_SIDE / CL_GLOBE_GROUP, (rows + CL_GLOBE_GROUP - 1) / CL_GLOBE_GROUP, 6);
    memoryBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT, kReaders,
                  VK_ACCESS_SHADER_READ_BIT);
}

void SceneClouds::recordView(SceneFrame& frame) {
    if (block_.viewSize[2] <= 0.0f) return;
    VkDescriptorSet set = writeSet(frame, false, false);
    if (!set) return;
    VkCommandBuffer cmd = frame.cmd;
    // (The cache's and the globe's bakes before it their own.)
    if (frame.timer) frame.timer->mark(cmd, "clouds:cache+globe");
    // What the last frame's march read of it is done with.
    memoryBarrier(cmd, kReaders, VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                  VK_ACCESS_SHADER_WRITE_BIT);
    bindScenePipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, current_->view.get());
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, computeLayout_, 0, 1, &set, 0, nullptr);
    const uint32_t w = static_cast<uint32_t>(block_.viewSize[0]), h = static_cast<uint32_t>(block_.viewSize[1]);
    vkCmdDispatch(cmd, (w + CL_VIEW_GROUP - 1) / CL_VIEW_GROUP, (h + CL_VIEW_GROUP - 1) / CL_VIEW_GROUP, 1);
    memoryBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT, kReaders,
                  VK_ACCESS_SHADER_READ_BIT);
    if (frame.timer) frame.timer->mark(cmd, "clouds:view");
}

namespace {

// Body direction `u` on face `f`'s equal-angle chart (-1 .. 1 within it;
// sky/cloud_heaps.glsl's cloudHeapFace, carried past the face's edges for a
// direction in its half of the sphere): false where `u` is not.
bool shellFaceAt(int f, const double u[3], double& e1, double& e2) {
    constexpr double q = 0.78539816339744831;
    const int axis = f / 2;
    const double a = f % 2 == 0 ? u[axis] : -u[axis];
    if (a < 0.05) return false;
    double x = 0.0, y = 0.0;
    switch (f) {
    case 0: x = -u[2]; y = u[1]; break;
    case 1: x = u[2]; y = u[1]; break;
    case 2: x = u[0]; y = u[2]; break;
    case 3: x = u[0]; y = -u[2]; break;
    case 4: x = u[0]; y = u[1]; break;
    default: x = -u[0]; y = u[1]; break;
    }
    e1 = std::atan(x / a) / q;
    e2 = std::atan(y / a) / q;
    return true;
}

// The face body direction `u` lies on (cloudHeapFace's).
int shellFaceOf(const double u[3]) {
    const double ax = std::fabs(u[0]), ay = std::fabs(u[1]), az = std::fabs(u[2]);
    if (ax >= ay && ax >= az) return u[0] >= 0.0 ? 0 : 1;
    if (ay >= az) return u[1] >= 0.0 ? 2 : 3;
    return u[2] >= 0.0 ? 4 : 5;
}

}  // namespace

void SceneClouds::placeShells(SceneFrame& frame, const float eye[3]) {
    const CloudSettings& c = frame.renderer.clouds();
    shellBoxes_.clear();
    std::memset(block_.shellEye, 0, sizeof(block_.shellEye));
    std::memset(block_.shellPlanes, 0, sizeof(block_.shellPlanes));
    std::memset(block_.shellParams, 0, sizeof(block_.shellParams));
    const double Rg = block_.shell[2], Rb = block_.shell[0], Rt = block_.shell[1];
    if (c.heapCell <= 0.0f || c.heapShells <= 0.0f || !shellsReady_) return;
    for (int i = 0; i < 3; ++i) block_.shellEye[i] = eye[i];
    block_.shellEye[3] = c.heapShells;
    block_.shellParams[0] = c.heapShells * static_cast<float>(CL_SHELL_BAND);
    // The farthest a heap's top is seen: over the horizon from the eye and
    // from the deck's top, the body's ground between; and no farther than
    // where a heap is a few pixels across, past which it is the heaps' share
    // (scene_cloud_layer.glsl's cloudHeapsFar, its footprint's end).
    const double Re = std::max(static_cast<double>(eyeRadius_), Rg);
    const double own = 0.12 * c.heapCell / std::max(block_.params[1], 1e-6f);
    const double horizon = std::min(std::sqrt(Re * Re - Rg * Rg) + std::sqrt(Rt * Rt - Rg * Rg), own);
    block_.shellParams[1] = static_cast<float>(horizon);
    // (The sort's bins from the nearest a heap can be: from over the deck,
    // the eye's height over its top.)
    block_.shellParams[2] = std::max(c.heapShells, static_cast<float>(eyeRadius_ - Rt));

    // The view's sides, through its corners (air space's axes), inward.
    double corner[4][3];
    const float ndc[4][2] = {{-1.0f, -1.0f}, {1.0f, -1.0f}, {1.0f, 1.0f}, {-1.0f, 1.0f}};
    double fwd[3] = {0.0, 0.0, 0.0};
    for (int k = 0; k < 4; ++k) {
        float v[4];
        for (int i = 0; i < 4; ++i)
            v[i] = frame.view.invProj.at(i, 0) * ndc[k][0] + frame.view.invProj.at(i, 1) * ndc[k][1] +
                   frame.view.invProj.at(i, 2) + frame.view.invProj.at(i, 3);
        for (int i = 0; i < 3; ++i) {
            corner[k][i] = 0.0;
            for (int j = 0; j < 3; ++j) corner[k][i] += frame.view.invRelView.at(i, j) * (v[j] / v[3]);
            fwd[i] += corner[k][i];
        }
    }
    for (int k = 0; k < 4; ++k) {
        const double* a = corner[k];
        const double* b = corner[(k + 1) % 4];
        double n[3] = {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]};
        const double l = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
        const double side = n[0] * fwd[0] + n[1] * fwd[1] + n[2] * fwd[2];
        for (int i = 0; i < 3; ++i) block_.shellPlanes[k][i] = static_cast<float>(n[i] / l * (side < 0.0 ? -1.0 : 1.0));
    }

    // The cells the deck can be seen over: a cap about the drifted nadir as
    // wide as the body's angle to that horizon, a face's box of cells for
    // each face the cap reaches (its rim sampled; past a face's edge on the
    // face's chart carried on).
    double nd[3];
    for (int i = 0; i < 3; ++i) {
        double b = 0.0;
        for (int j = 0; j < 3; ++j) b += static_cast<double>(block_.toBody[i][j]) * eye[j];
        nd[i] = b / Re * Rb - static_cast<double>(block_.wind[i]) * block_.shell[3];
    }
    const double nl = std::sqrt(nd[0] * nd[0] + nd[1] * nd[1] + nd[2] * nd[2]);
    for (double& v : nd) v /= nl;
    // (Or as wide as the farthest a heap is drawn: a point of the deck that
    // far round is at least that far off.)
    const double psi = std::min({std::acos(std::min(Rg / Re, 1.0)) + std::acos(std::min(Rg / Rt, 1.0)), 0.6,
                                 (horizon + 2.0 * c.heapCell) / Rb});
    double a1[3] = {1.0, 0.0, 0.0};
    if (std::fabs(nd[0]) > 0.9) a1[0] = 0.0, a1[1] = 1.0;
    const double an = a1[0] * nd[0] + a1[1] * nd[1] + a1[2] * nd[2];
    for (int i = 0; i < 3; ++i) a1[i] -= an * nd[i];
    const double l1 = std::sqrt(a1[0] * a1[0] + a1[1] * a1[1] + a1[2] * a1[2]);
    for (double& v : a1) v /= l1;
    const double a2[3] = {nd[1] * a1[2] - nd[2] * a1[1], nd[2] * a1[0] - nd[0] * a1[2], nd[0] * a1[1] - nd[1] * a1[0]};
    constexpr int kRim = 128;
    double pts[kRim + 1][3];
    for (int i = 0; i < 3; ++i) pts[kRim][i] = nd[i];
    for (int k = 0; k < kRim; ++k) {
        const double th = 6.283185307179586 * k / kRim;
        for (int i = 0; i < 3; ++i)
            pts[k][i] = std::cos(psi) * nd[i] + std::sin(psi) * (std::cos(th) * a1[i] + std::sin(th) * a2[i]);
    }
    const int n = std::max(static_cast<int>(std::floor(Rb * 1.5707963 / std::max(c.heapCell, 1.0f) + 0.5)), 1);
    for (int f = 0; f < 6; ++f) {
        bool reached = false;
        double lo[2] = {1.0, 1.0}, hi[2] = {-1.0, -1.0};
        for (const auto& p : pts) {
            if (shellFaceOf(p) == f) reached = true;
            double e1, e2;
            if (!shellFaceAt(f, p, e1, e2)) continue;
            lo[0] = std::min(lo[0], e1), hi[0] = std::max(hi[0], e1);
            lo[1] = std::min(lo[1], e2), hi[1] = std::max(hi[1], e2);
        }
        if (!reached) continue;
        int i0 = static_cast<int>(std::floor((std::max(lo[0], -1.0) * 0.5 + 0.5) * n)) - 2;
        int j0 = static_cast<int>(std::floor((std::max(lo[1], -1.0) * 0.5 + 0.5) * n)) - 2;
        int i1 = static_cast<int>(std::floor((std::min(hi[0], 1.0) * 0.5 + 0.5) * n)) + 2;
        int j1 = static_cast<int>(std::floor((std::min(hi[1], 1.0) * 0.5 + 0.5) * n)) + 2;
        i0 = std::max(i0, 0), j0 = std::max(j0, 0), i1 = std::min(i1, n - 1), j1 = std::min(j1, n - 1);
        if (i1 >= i0 && j1 >= j0) shellBoxes_.push_back({f, i0, j0, i1 - i0 + 1, j1 - j0 + 1});
    }
}

void SceneClouds::recordShells(SceneFrame& frame) {
    if (block_.shellEye[3] <= 0.0f || !current_->shells.get() || !shellsSortPipeline_.get()) return;
    VkCommandBuffer cmd = frame.cmd;
    // What the last frame drew from it is done with; its head cleared.
    memoryBarrier(cmd, VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT | VK_PIPELINE_STAGE_VERTEX_SHADER_BIT |
                  VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_INDIRECT_COMMAND_READ_BIT | VK_ACCESS_SHADER_READ_BIT,
                  VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
    vkCmdFillBuffer(cmd, shells_.buffer, 0, kShellHeadBytes, 0);
    memoryBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                  VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
    VkDescriptorSet set = writeSet(frame, false, false);
    if (!set) return;
    // The plan, a face's box of cells a dispatch.
    bindScenePipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, current_->shells.get());
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, computeLayout_, 0, 1, &set, 0, nullptr);
    for (const ShellBox& b : shellBoxes_) {
        const int32_t push[8] = {b.face, b.i0, b.j0, 0, b.ni, b.nj, 0, 0};
        vkCmdPushConstants(cmd, computeLayout_, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), push);
        vkCmdDispatch(cmd, static_cast<uint32_t>((b.ni + CL_SHELL_GROUP - 1) / CL_SHELL_GROUP),
                      static_cast<uint32_t>((b.nj + CL_SHELL_GROUP - 1) / CL_SHELL_GROUP), 1);
    }
    // The sort: the bins laid out, then each heap moved into its bin.
    bindScenePipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, shellsSortPipeline_.get());
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, computeLayout_, 0, 1, &set, 0, nullptr);
    for (int mode = 0; mode < 2; ++mode) {
        memoryBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT,
                      VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
        const int32_t push[8] = {mode, 0, 0, 0, 0, 0, 0, 0};
        vkCmdPushConstants(cmd, computeLayout_, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), push);
        vkCmdDispatch(cmd, mode == 0 ? 1u : static_cast<uint32_t>(CL_SHELL_MOST / CL_SHELL_SORT_GROUP), 1, 1);
    }
    memoryBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT,
                  VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT | VK_PIPELINE_STAGE_VERTEX_SHADER_BIT |
                  VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_INDIRECT_COMMAND_READ_BIT | VK_ACCESS_SHADER_READ_BIT);
    if (frame.timer) frame.timer->mark(cmd, "clouds:shells-plan");
}

void SceneClouds::drawShells(SceneFrame& frame, VkDescriptorSet set) {
    // (Asked for every frame the deck is drawn, so the shells are on, and
    // the march hands its heaps over, only once all of theirs are in.)
    VkPipeline pipeline = shellPipelines_.get(0, frame.hdrTarget, [&] {
        // Premultiplied over what is drawn, as the march.
        VkPipelineColorBlendAttachmentState over{};
        over.blendEnable = VK_TRUE;
        over.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
        over.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        over.colorBlendOp = VK_BLEND_OP_ADD;
        over.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        over.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        over.alphaBlendOp = VK_BLEND_OP_ADD;
        over.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT |
                              VK_COLOR_COMPONENT_A_BIT;
        SceneVkPipelineBuilder b;
        b.setShaderStages(shellsVs_, shellsFs_)
         .setCullMode(VK_CULL_MODE_NONE)
         .setTarget(frame.hdrTarget)
         .disableBlending(frame.hdrTarget.colorCount)
         .setColorBlendAttachment(0, over)
         .disableDepthTest();
        return b.build(device_->device(), marchLayout_);
    }, /*async=*/true);
    shellsReady_ = pipeline && current_->shells.get() && shellsSortPipeline_.get();
    if (!shellsReady_ || block_.shellEye[3] <= 0.0f || shellBoxes_.empty()) return;
    const VkDescriptorSet sets[3] = {frame.cameraSet, frame.lightingSet, set};
    bindScenePipeline(frame.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
    vkCmdBindDescriptorSets(frame.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, marchLayout_, 0, 3, sets, 0, nullptr);
    vkCmdDrawIndirect(frame.cmd, shells_.buffer, CL_SHELL_H_ARGS * sizeof(uint32_t), 1, sizeof(VkDrawIndirectCommand));
    frame.drewContent = true;
    if (frame.timer) frame.timer->mark(frame.cmd, "clouds:shells");
}

void SceneClouds::recordDraw(SceneFrame& frame) {
    if (!on_ || !current_) return;
    // (The debug variant, sky/cloud_volume_debug.glsl, under BRO_CLOUD_VOLUME_DEBUG.)
    const bool debug = marchVolumeFs_ != VK_NULL_HANDLE;
    VkPipeline pipeline = marchPipelines_.get(debug ? 1 : 0, frame.hdrTarget, [&] {
        // Premultiplied over what is drawn: the deck's light and the air
        // before it added, what is behind it dimmed by what it lets through.
        VkPipelineColorBlendAttachmentState over{};
        over.blendEnable = VK_TRUE;
        over.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
        over.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        over.colorBlendOp = VK_BLEND_OP_ADD;
        over.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        over.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        over.alphaBlendOp = VK_BLEND_OP_ADD;
        over.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT |
                              VK_COLOR_COMPONENT_A_BIT;
        SceneVkPipelineBuilder b;
        b.setShaderStages(skyVs_, debug ? marchVolumeFs_ : marchFs_)
         .setCullMode(VK_CULL_MODE_NONE)
         .setTarget(frame.hdrTarget)
         .disableBlending(frame.hdrTarget.colorCount)
         .setColorBlendAttachment(0, over)
         .disableDepthTest();
        return b.build(device_->device(), marchLayout_);
    }, /*async=*/true);
    VkDescriptorSet set = writeSet(frame, true, false);
    if (!set) return;
    drawShells(frame, set);
    if (!pipeline) return;
    const VkDescriptorSet sets[3] = {frame.cameraSet, frame.lightingSet, set};
    bindScenePipeline(frame.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
    vkCmdBindDescriptorSets(frame.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, marchLayout_, 0, 3, sets, 0, nullptr);
    vkCmdDraw(frame.cmd, 3, 1, 0, 0);
    frame.drewContent = true;
}

namespace {
float coverHalf(uint16_t h) {
    const uint32_t sign = static_cast<uint32_t>(h & 0x8000u) << 16;
    const uint32_t exp = (h >> 10) & 0x1Fu;
    const uint32_t man = h & 0x3FFu;
    if (exp == 0) return (sign ? -1.0f : 1.0f) * std::ldexp(static_cast<float>(man), -24);
    if (exp == 31) return std::bit_cast<float>(sign | 0x7F800000u | (man << 13));
    return std::bit_cast<float>(sign | ((exp + 112u) << 23) | (man << 13));
}
}  // namespace

bool SceneClouds::readCover(SceneGpu& gpu, std::vector<float>& out) {
    if (!cover_.isValid() || !coverHeld_) return false;
    SceneVkDevice& device = gpu.device;
    device.waitIdle();
    const size_t texels = static_cast<size_t>(CL_COVER_SIDE) * CL_COVER_SIDE * 6;
    SceneVkBuffer buffer;
    if (!gpu.allocator.createBuffer(texels * 8, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                    VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                                    buffer) ||
        !buffer.mappedData) {
        gpu.allocator.destroyBuffer(buffer);
        return false;
    }
    VkCommandBuffer cmd = device.frames().beginCommands();
    render::cmdMemoryBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_ACCESS_SHADER_WRITE_BIT,
                             VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT);
    VkBufferImageCopy region{};
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 6};
    region.imageExtent = {static_cast<uint32_t>(CL_COVER_SIDE), static_cast<uint32_t>(CL_COVER_SIDE), 1};
    vkCmdCopyImageToBuffer(cmd, cover_.image, VK_IMAGE_LAYOUT_GENERAL, buffer.buffer, 1, &region);
    render::cmdMemoryBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
                             VK_PIPELINE_STAGE_HOST_BIT, VK_ACCESS_HOST_READ_BIT);
    const uint64_t ticket = device.frames().submit(cmd);
    const bool ok = ticket != 0 && device.context().queue().wait(ticket);
    if (ok) {
        const auto* src = static_cast<const uint16_t*>(buffer.mappedData);
        out.resize(texels * 4);
        for (size_t i = 0; i < texels * 4; ++i) out[i] = coverHalf(src[i]);
    }
    gpu.allocator.destroyBuffer(buffer);
    return ok;
}

void SceneClouds::coverAt(const std::vector<float>& cube, const double up[3], float out[4]) {
    out[0] = out[1] = out[2] = out[3] = 0.0f;
    constexpr int S = CL_COVER_SIDE;
    if (cube.size() < static_cast<size_t>(S) * S * 6 * 4) return;
    // (sky/scene_cloud_layer.glsl's cloudCubeCoord.)
    const double ax = std::abs(up[0]), ay = std::abs(up[1]), az = std::abs(up[2]);
    int f;
    double u, v, n;
    if (ax >= ay && ax >= az) {
        f = up[0] >= 0.0 ? 0 : 1;
        u = up[0] >= 0.0 ? -up[2] : up[2];
        v = up[1];
        n = ax;
    } else if (ay >= az) {
        f = up[1] >= 0.0 ? 2 : 3;
        u = up[0];
        v = up[1] >= 0.0 ? up[2] : -up[2];
        n = ay;
    } else {
        f = up[2] >= 0.0 ? 4 : 5;
        u = up[2] >= 0.0 ? up[0] : -up[0];
        v = up[1];
        n = az;
    }
    if (!(n > 0.0)) return;
    constexpr double kQuarterPi = 0.78539816339744831;
    const double eu = std::atan(u / n) / kQuarterPi, ev = std::atan(v / n) / kQuarterPi;
    // Texel space, centres at i + 0.5, bilinear as textureLod.
    const double tx = (eu * 0.5 + 0.5) * (S - 2.0) + 1.0 - 0.5;
    const double ty = (ev * 0.5 + 0.5) * (S - 2.0) + 1.0 - 0.5;
    const int x0 = std::clamp(static_cast<int>(std::floor(tx)), 0, S - 2);
    const int y0 = std::clamp(static_cast<int>(std::floor(ty)), 0, S - 2);
    const double fx = std::clamp(tx - x0, 0.0, 1.0), fy = std::clamp(ty - y0, 0.0, 1.0);
    const float* face = cube.data() + static_cast<size_t>(f) * S * S * 4;
    for (int c = 0; c < 4; ++c) {
        auto at = [&](int x, int y) { return static_cast<double>(face[(static_cast<size_t>(y) * S + x) * 4 + c]); };
        const double top = at(x0, y0) * (1.0 - fx) + at(x0 + 1, y0) * fx;
        const double bot = at(x0, y0 + 1) * (1.0 - fx) + at(x0 + 1, y0 + 1) * fx;
        out[c] = static_cast<float>(top * (1.0 - fy) + bot * fy);
    }
}

}  // namespace bro::scene::vk
