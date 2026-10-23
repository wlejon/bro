#include "scene/vulkan/sky/scene_clouds.h"

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
#include <cmath>
#include <cstring>

namespace bro::scene::vk {

static_assert(sizeof(SceneClouds::Block) == 16 * (3 + 6 + 4 + CL_CACHE_LEVELS + 2 + CL_MAX_DATA));
static_assert(CloudSettings::kMaxData == CL_MAX_DATA);
static_assert(CL_CACHE_LEVELS == 13 && CL_CACHE_SIDE % (CL_CACHE_GROUP * CL_CACHE_SLICES) == 0);

namespace {

constexpr VkFormat kCacheFormat = VK_FORMAT_R16G16B16A16_SFLOAT;
constexpr VkFormat kSigmaFormat = VK_FORMAT_R16G16B16A16_SFLOAT;
constexpr VkFormat kDistFormat = VK_FORMAT_R16_SFLOAT;
constexpr VkFormat kCoverFormat = VK_FORMAT_R16G16B16A16_SFLOAT;
constexpr VkFormat kShadowFormat = VK_FORMAT_R32_SFLOAT;
constexpr VkFormat kGridFormat = VK_FORMAT_R32G32B32A32_SFLOAT;
// (Half floats: the mips' variance, E[x^2] - E[x]^2, is a small difference.)
constexpr VkFormat kNoiseFormat = VK_FORMAT_R8G8B8A8_UNORM;
constexpr VkShaderStageFlags kStages = VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_COMPUTE_BIT;

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
    setLayout_ = set.build(dev);
    SceneVkDescriptorLayoutBuilder noise;
    noise.addBinding(0, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT);
    noise.addBinding(1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT);
    noiseSetLayout_ = noise.build(dev);
    if (!setLayout_ || !noiseSetLayout_) return false;
    marchLayout_ = pipelineLayout(dev, {gpu.defaults.cameraLayout, gpu.defaults.lightingLayout, setLayout_}, 0);
    computeLayout_ = pipelineLayout(dev, {setLayout_}, 4 * sizeof(int32_t));
    noiseLayout_ = pipelineLayout(dev, {noiseSetLayout_}, 2 * sizeof(int32_t));
    skyVs_ = SceneVkShaderCompiler::createBuiltinModule(dev, BuiltinSceneShader::SkyVert);
    marchFs_ = SceneVkShaderCompiler::createBuiltinModule(dev, BuiltinSceneShader::CloudsFrag);
    noiseCs_ = SceneVkShaderCompiler::createBuiltinModule(dev, BuiltinSceneShader::CloudsNoiseComp);
    lightCs_ = SceneVkShaderCompiler::createBuiltinModule(dev, BuiltinSceneShader::CloudsLightComp);
    shadowCs_ = SceneVkShaderCompiler::createBuiltinModule(dev, BuiltinSceneShader::CloudsShadowComp);
    noisePipeline_ = noiseCs_ && noiseLayout_ ? createComputePipeline(dev, noiseCs_, noiseLayout_, "clouds noise")
                                              : VK_NULL_HANDLE;
    distCs_ = SceneVkShaderCompiler::createBuiltinModule(dev, BuiltinSceneShader::CloudsDistComp);
    if (lightCs_ && computeLayout_) lightPipeline_ = createComputePipeline(dev, lightCs_, computeLayout_, "clouds light");
    if (distCs_ && computeLayout_) distPipeline_ = createComputePipeline(dev, distCs_, computeLayout_, "clouds dist");
    if (shadowCs_ && computeLayout_)
        shadowPipeline_ = createComputePipeline(dev, shadowCs_, computeLayout_, "clouds shadow");
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
    if (!marchLayout_ || !computeLayout_ || !noiseLayout_ || !skyVs_ || !marchFs_ || !noisePipeline_ || !lightPipeline_ ||
        !distPipeline_ ||
        !shadowPipeline_ || !noiseSampler_ || !coverSampler_ || !cacheSampler_ ||
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
        if (s->cover) vkDestroyPipeline(dev, s->cover, nullptr);
        if (s->cache) vkDestroyPipeline(dev, s->cache, nullptr);
        s->destroy(dev);
    }
    sets_.clear();
    byHash_.clear();
    current_ = nullptr;
    chartValid_ = false;
    for (SceneVkImage* i :
         {&cover_, &shadow_, &coverNone_, &shadowNone_, &gridNone_, &cacheSigma_, &cacheLight_, &cacheBand_, &distA_,
          &distB_, &cacheHolds_})
        if (i->isValid()) gpu.allocator.destroyImage(*i);
    if (noise_.image) {
        for (VkImageView v : noise_.mips)
            if (v) vkDestroyImageView(dev, v, nullptr);
        if (noise_.sampled) vkDestroyImageView(dev, noise_.sampled, nullptr);
        vkDestroyImage(dev, noise_.image, nullptr);
        if (noise_.allocId) gpu.device.context().memoryPool().free(dev, noise_.allocId);
    }
    noise_ = Noise{};
    for (VkPipeline* p : {&noisePipeline_, &lightPipeline_, &distPipeline_, &shadowPipeline_})
        if (*p) vkDestroyPipeline(dev, *p, nullptr);
    lightPipeline_ = distPipeline_ = shadowPipeline_ = VK_NULL_HANDLE;
    SceneVkShaderModule::destroy(dev, noiseCs_);
    SceneVkShaderModule::destroy(dev, lightCs_);
    SceneVkShaderModule::destroy(dev, distCs_);
    SceneVkShaderModule::destroy(dev, shadowCs_);
    lightCs_ = distCs_ = shadowCs_ = VK_NULL_HANDLE;
    marchPipelines_.destroy(gpu.device);
    SceneVkShaderModule::destroy(dev, skyVs_);
    SceneVkShaderModule::destroy(dev, marchFs_);
    marchFs_ = VK_NULL_HANDLE;
    for (VkPipelineLayout* l : {&marchLayout_, &computeLayout_, &noiseLayout_})
        if (*l) vkDestroyPipelineLayout(dev, *l, nullptr);
    for (VkDescriptorSetLayout* l : {&setLayout_, &noiseSetLayout_})
        if (*l) vkDestroyDescriptorSetLayout(dev, *l, nullptr);
    for (VkSampler* s : {&noiseSampler_, &coverSampler_, &cacheSampler_})
        if (*s) vkDestroySampler(dev, *s, nullptr);
    noisePipeline_ = VK_NULL_HANDLE;
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
        // Compiled once a module (the SPIR-V cache keeps it across runs);
        // one that does not compile is logged once and draws no clouds.
        auto set = std::make_unique<CloudShaders>();
        std::string err;
        int index = -1;
        VkDevice dev = device_->device();
        if (set->create(dev, c.source, grid, err)) {
            set->cover = createComputePipeline(dev, (*set)[CloudShaders::Cover], computeLayout_, "clouds cover");
            set->cache = createComputePipeline(dev, (*set)[CloudShaders::Cache], computeLayout_, "clouds cache");
            if (set->cover && set->cache) {
                index = static_cast<int>(sets_.size());
                set->index = static_cast<uint32_t>(index);
                sets_.push_back(std::move(set));
            } else {
                err = "its compute pipelines could not be made";
            }
        }
        if (index < 0) LOG_ERROR("The clouds' module did not compile, so no clouds are drawn:\n%s", err.c_str());
        it = byHash_.emplace(key, index).first;
    }
    currentKey_ = key;
    return it->second < 0 ? nullptr : sets_[static_cast<size_t>(it->second)].get();
}

bool SceneClouds::ensureImages(SceneGpu& gpu) {
    if (cover_.isValid()) return true;
    SceneVkAllocator& a = gpu.allocator;
    const uint32_t cacheDepth = CL_CACHE_LEVELS * CL_CACHE_BINS;
    if (!a.createStorageImage(CL_COVER_SIDE, CL_COVER_SIDE, 1, kCoverFormat, cover_, false, 6) ||
        !a.createStorageImage(CL_SHADOW_SIDE, CL_SHADOW_SIDE, 1, kShadowFormat, shadow_, false, CL_SHADOW_LEVELS) ||
        !a.createStorageImage(CL_CACHE_SIDE, CL_CACHE_SIDE, cacheDepth, kSigmaFormat, cacheSigma_, false) ||
        !a.createStorageImage(CL_CACHE_SIDE, CL_CACHE_SIDE, cacheDepth, kCacheFormat, cacheLight_, false) ||
        !a.createStorageImage(CL_CACHE_SIDE, CL_CACHE_SIDE, 1, kCacheFormat, cacheBand_, false, 2 * CL_CACHE_LEVELS) ||
        !a.createStorageImage(CL_CACHE_SIDE, CL_CACHE_SIDE, cacheDepth, kDistFormat, distA_, false) ||
        !a.createStorageImage(CL_CACHE_SIDE, CL_CACHE_SIDE, cacheDepth, kDistFormat, distB_, false) ||
        !a.createStorageImage(CL_CACHE_SIDE, CL_CACHE_SIDE, cacheDepth, kDistFormat, cacheHolds_, false) ||
        !a.createStorageImage(1, 1, 1, kCoverFormat, coverNone_, false, 1) ||
        !a.createStorageImage(1, 1, 1, kGridFormat, gridNone_, false, 1) || !createNoise(gpu)) {
        LOG_ERROR("SceneClouds: failed creating the clouds' images");
        for (SceneVkImage* i :
             {&cover_, &shadow_, &coverNone_, &gridNone_, &cacheSigma_, &cacheLight_, &cacheBand_, &distA_, &distB_,
              &cacheHolds_})
            if (i->isValid()) a.destroyImage(*i);
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
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, noisePipeline_);
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
    stats.compiled = current_ != nullptr;
    stats.failed = current_ == nullptr;
    if (!current_ || !ensureImages(frame.gpu)) {
        frame.renderer.setCloudStats(stats);
        return;
    }

    // The cover: baked again for a new generation or module, once the
    // land it reads (when it reads one) is ready.
    if (bakedGeneration_ != c.generation || bakedHash_ != currentKey_) {
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
        bake_ = true;
    }
    stats.covered = true;
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
    block_.wind[3] = 0.0f;
    const size_t vecs = std::min(c.data.size() / 4, static_cast<size_t>(CL_MAX_DATA));
    std::memset(block_.data, 0, sizeof(block_.data));
    std::memcpy(block_.data, c.data.data(), vecs * 4 * sizeof(float));

    // The shadow's levels: about the body's point under the eye, on the
    // plane tangent there.
    float eye[4];
    atm.eyeAbout(frame.world.eye, eye);
    const float el = std::sqrt(eye[0] * eye[0] + eye[1] * eye[1] + eye[2] * eye[2]);
    const float n[3] = {eye[0] / el, eye[1] / el, eye[2] / el};
    float a[3] = {1.0f, 0.0f, 0.0f};
    if (std::fabs(n[0]) > 0.9f) a[0] = 0.0f, a[2] = 1.0f;
    const float an = a[0] * n[0] + a[1] * n[1] + a[2] * n[2];
    float e1[3] = {a[0] - an * n[0], a[1] - an * n[1], a[2] - an * n[2]};
    const float l1 = std::sqrt(e1[0] * e1[0] + e1[1] * e1[1] + e1[2] * e1[2]);
    for (float& v : e1) v /= l1;
    const float e2[3] = {n[1] * e1[2] - n[2] * e1[1], n[2] * e1[0] - n[0] * e1[2], n[0] * e1[1] - n[1] * e1[0]};
    for (int k = 0; k < 3; ++k) {
        block_.shadowN[k] = shadowBlock_.n[k] = n[k];
        block_.shadowE1[k] = shadowBlock_.e1[k] = e1[k];
        block_.shadowE2[k] = shadowBlock_.e2[k] = e2[k];
    }
    block_.shadowN[3] = CL_SHADOW_GROWTH;
    block_.shadowE1[3] = CL_SHADOW_TEXEL0;
    shadowsOn_ = c.shadows;
    shadowBlock_.params[0] = shadowsOn_ ? 1.0f : 0.0f;
    shadowBlock_.params[1] = R;
    shadowBlock_.params[2] = CL_SHADOW_TEXEL0;
    shadowBlock_.params[3] = CL_SHADOW_GROWTH;
    placeCache(frame);
}

void SceneClouds::placeCache(SceneFrame& frame) {
    const CloudSettings& c = frame.renderer.clouds();
    const double Rb = block_.shell[0];
    // The nadir in the body's frame.
    double nb[3];
    for (int i = 0; i < 3; ++i)
        nb[i] = block_.toBody[i][0] * block_.shadowN[0] + block_.toBody[i][1] * block_.shadowN[1] +
                block_.toBody[i][2] * block_.shadowN[2];
    const double nl = std::sqrt(nb[0] * nb[0] + nb[1] * nb[1] + nb[2] * nb[2]);
    for (double& v : nb) v /= nl;
    const auto dot = [](const double* a, const double* b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; };

    const uint64_t key = mixBits(currentKey_, c.generation);
    bool all = !chartValid_ || key != cacheKey_ || bake_;
    // (A frame's jump in the wind's time: a deck that moved, not drifted.)
    all = all || std::fabs(block_.shell[3] - cacheTime_) > 0.25f;
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
    }
    cacheKey_ = key;
    // The nadir on the chart.
    const double vx = dot(nb, chartE1_), vy = dot(nb, chartE2_);
    const double sv = std::sqrt(vx * vx + vy * vy);
    const double th = std::atan2(sv, dot(nb, chartC_));
    const double scale = sv > 1e-12 ? th * Rb / sv : Rb;
    const double x = vx * scale, y = vy * scale;
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
    // moved is baked whole; every level's rows a share a frame.
    bakes_.clear();
    for (int k = 0; k < CL_CACHE_LEVELS; ++k) {
        const double cell = CL_CACHE_TEXEL0 * std::ldexp(1.0, k);
        const long long cx = static_cast<long long>(std::floor(x / cell));
        const long long cy = static_cast<long long>(std::floor(y / cell));
        if (!all && (cx != levelCentre_[k][0] || cy != levelCentre_[k][1])) bakes_.push_back({k, 1, 0, 1});
        levelCentre_[k][0] = cx;
        levelCentre_[k][1] = cy;
        block_.cacheLevel[k][0] = static_cast<float>(cx);
        block_.cacheLevel[k][1] = static_cast<float>(cy);
        block_.cacheLevel[k][2] = block_.cacheLevel[k][3] = 0.0f;
    }
    if (all) {
        bakes_.assign(1, {0, CL_CACHE_LEVELS, 0, 1});
    } else {
        bakes_.push_back({0, CL_CACHE_LEVELS, static_cast<int>(cacheFrame_ % CL_CACHE_SLICES), CL_CACHE_SLICES});
    }
    ++cacheFrame_;
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
    w.updateSet(device_->device(), set);
    return set;
}

void SceneClouds::recordPrepare(SceneFrame& frame) {
    if (!on_ || !current_) return;
    VkCommandBuffer cmd = frame.cmd;
    const AtmosphereGpu& atm = frame.lighting.atm;
    for (int k = 0; k < 3; ++k) block_.sun[k] = atm.sunDir[k];
    block_.sun[3] = atm.sunIlluminance[3];
    if (!noise_.made) recordNoise(frame);

    if (bake_) {
        VkDescriptorSet set = writeSet(frame, false, true);
        if (!set) return;
        // The land's grid its processes wrote, and the cover's last readers.
        memoryBarrier(cmd, kReaders | VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_SHADER_WRITE_BIT |
                      VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                      VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, current_->cover);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, computeLayout_, 0, 1, &set, 0, nullptr);
        const uint32_t groups = (CL_COVER_SIDE + CL_COVER_GROUP - 1) / CL_COVER_GROUP;
        vkCmdDispatch(cmd, groups, groups, 6);
        memoryBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT, kReaders,
                      VK_ACCESS_SHADER_READ_BIT);
        bakedGeneration_ = frame.renderer.clouds().generation;
        bakedHash_ = currentKey_;
        bake_ = false;
    }

    recordCache(frame);

    if (shadowsOn_) {
        VkDescriptorSet set = writeSet(frame, false, false);
        if (!set) return;
        // What earlier frames read of the levels is done with.
        memoryBarrier(cmd, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT,
                      VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, shadowPipeline_);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, computeLayout_, 0, 1, &set, 0, nullptr);
        const uint32_t groups = (CL_SHADOW_SIDE + CL_SHADOW_GROUP - 1) / CL_SHADOW_GROUP;
        vkCmdDispatch(cmd, groups, groups, CL_SHADOW_LEVELS);
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
    if (turned < 0.99996f) bakes_.assign(1, {0, CL_CACHE_LEVELS, 0, 1});   // (half a degree)
    std::memcpy(cacheSun_, sun, sizeof(sun));
    if (bakes_.empty()) return;
    VkDescriptorSet set = writeSet(frame, false, false);
    if (!set) return;
    // What earlier frames read of the cache is done with; the cover baked.
    memoryBarrier(cmd, kReaders, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT,
                  VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
    // The extinction, its light, then its distance to cloud an axis a pass.
    for (int pass = 0; pass < 5; ++pass) {
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                          pass == 0 ? current_->cache : pass == 1 ? lightPipeline_ : distPipeline_);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, computeLayout_, 0, 1, &set, 0, nullptr);
        for (const CacheBake& b : bakes_) {
            const int32_t push[4] = {b.first, b.y0, b.yStep, std::max(pass - 2, 0)};
            vkCmdPushConstants(cmd, computeLayout_, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), push);
            const uint32_t rows = static_cast<uint32_t>((CL_CACHE_SIDE - b.y0 + b.yStep - 1) / b.yStep);
            vkCmdDispatch(cmd, CL_CACHE_SIDE / CL_CACHE_GROUP, (rows + CL_CACHE_GROUP - 1) / CL_CACHE_GROUP,
                          static_cast<uint32_t>(b.levels * CL_CACHE_BINS));
        }
        memoryBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT,
                      pass < 4 ? VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT : kReaders, VK_ACCESS_SHADER_READ_BIT);
    }
}

void SceneClouds::recordDraw(SceneFrame& frame) {
    if (!on_ || !current_) return;
    VkPipeline pipeline = marchPipelines_.get(0, frame.hdrTarget, [&] {
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
        b.setShaderStages(skyVs_, marchFs_)
         .setCullMode(VK_CULL_MODE_NONE)
         .setTarget(frame.hdrTarget)
         .disableBlending(frame.hdrTarget.colorCount)
         .setColorBlendAttachment(0, over)
         .disableDepthTest();
        return b.build(device_->device(), marchLayout_);
    });
    VkDescriptorSet set = writeSet(frame, true, false);
    if (!pipeline || !set) return;
    const VkDescriptorSet sets[3] = {frame.cameraSet, frame.lightingSet, set};
    vkCmdBindPipeline(frame.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
    vkCmdBindDescriptorSets(frame.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, marchLayout_, 0, 3, sets, 0, nullptr);
    vkCmdDraw(frame.cmd, 3, 1, 0, 0);
    frame.drewContent = true;
}

}  // namespace bro::scene::vk
