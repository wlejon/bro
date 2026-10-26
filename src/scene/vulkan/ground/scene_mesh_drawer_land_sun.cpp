// The land's sun (scene/world/land_sun.h; docs/scene-world-architecture.md
// §5, "The land's sun"): its levels about the eye, made from the first land
// offered's tiles by land_sun.comp as the world service grants them, a
// level a job, and read by every lit shader through the lighting set
// (scene_lighting.glsl's sceneLandSun).

#include "scene/vulkan/draw/scene_mesh_drawer.h"
#include "scene/vulkan/frame/scene_shader_sets.h"

#include "scene/nodes/land_node.h"
#include "scene/scene_graph.h"
#include "scene/vulkan/frame/scene_frame.h"
#include "scene/vulkan/frame/scene_vk_descriptors.h"
#include "scene/vulkan/frame/scene_vk_device.h"
#include "scene/vulkan/frame/scene_vk_pipeline.h"
#include "scene/vulkan/frame/scene_vk_shader_compiler.h"
#include "scene/vulkan/shaders/ground/land_sun_layout.h"
#include "scene/vulkan/shaders/ground/scene_land_layout.h"

#include "util/log.h"

#include <cstring>

namespace bro::scene::vk {

namespace {

static_assert(LandSun::kLevels == LS_LEVELS && LandSun::kSide == LS_SIDE && LandSun::kTexel0 == LS_TEXEL0);

constexpr VkFormat kLandSunFormat = VK_FORMAT_R32G32_SFLOAT;

uint64_t mixBits(uint64_t h, uint64_t v) {
    h ^= v + 0x9E3779B97F4A7C15ull + (h << 6) + (h >> 2);
    return h;
}

uint64_t floatBits(float f) {
    uint32_t u;
    std::memcpy(&u, &f, sizeof(u));
    return u;
}

}  // namespace

bool SceneMeshDrawer::setupLandSun(SceneGpu& gpu) {
    // Its set: the LandDraw words, the tiles' heights, surface, page table
    // and canopy, and the levels.
    SceneVkDescriptorLayoutBuilder set;
    set.addBinding(0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT);
    for (uint32_t b = 1; b <= 4; ++b)
        set.addBinding(b, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT);
    set.addBinding(5, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT);
    landSunSetLayout_ = set.build(device_);
    const VkPushConstantRange push{VK_SHADER_STAGE_COMPUTE_BIT, 0, LS_P_WORDS * sizeof(float)};
    VkPipelineLayoutCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    info.setLayoutCount = 1;
    info.pSetLayouts = &landSunSetLayout_;
    info.pushConstantRangeCount = 1;
    info.pPushConstantRanges = &push;
    if (!landSunSetLayout_ || vkCreatePipelineLayout(device_, &info, nullptr, &landSunLayout_) != VK_SUCCESS) {
        LOG_ERROR("SceneMeshDrawer: failed creating the land's sun's layout");
        return false;
    }
    landSunModule_ = SceneVkShaderCompiler::createBuiltinModule(device_, BuiltinSceneShader::LandSunComp);
    landSunPipeline_ =
        landSunModule_ ? createComputePipeline(device_, landSunModule_, landSunLayout_, "land sun") : VK_NULL_HANDLE;
    // What the lighting set binds until a land makes the levels (in GENERAL,
    // as they are).
    if (!landSunPipeline_ || !gpu.allocator.createStorageImage(1, 1, 1, kLandSunFormat, landSunNone_, false, 1)) {
        LOG_ERROR("SceneMeshDrawer: failed creating the land's sun's pipeline");
        return false;
    }
    // Recorded after the land's fills, which it reads.
    if (landSunKind_ < 0) landSunKind_ = gpu.world.addKind(this, {"land sun", WorldSchedule::Raster, 0.05f});
    if (landSunKind_ < 0) {
        LOG_ERROR("SceneMeshDrawer: the world service has no room for the land's sun");
        return false;
    }
    return true;
}

void SceneMeshDrawer::cleanupLandSun(SceneGpu& gpu) {
    if (landSunImage_.isValid()) gpu.allocator.destroyImage(landSunImage_);
    if (landSunNone_.isValid()) gpu.allocator.destroyImage(landSunNone_);
    if (landSunPipeline_) destroyScenePipeline(device_, landSunPipeline_, nullptr);
    SceneVkShaderModule::destroy(device_, landSunModule_);
    if (landSunLayout_) vkDestroyPipelineLayout(device_, landSunLayout_, nullptr);
    if (landSunSetLayout_) vkDestroyDescriptorSetLayout(device_, landSunSetLayout_, nullptr);
    landSunPipeline_ = VK_NULL_HANDLE;
    landSunModule_ = VK_NULL_HANDLE;
    landSunLayout_ = VK_NULL_HANDLE;
    landSunSetLayout_ = VK_NULL_HANDLE;
    landSun_.forget();
}

void SceneMeshDrawer::fillLandSun(LandSunGpu& out, const bromath::Vec3& eye) const {
    out = LandSunGpu{};
    bool any = false;
    for (int l = 0; l < LS_LEVELS; ++l) {
        const LandSun::Level& lv = landSun_.level(l);
        out.level[l][0] = static_cast<float>(lv.x0 - eye.x);
        out.level[l][1] = static_cast<float>(lv.z0 - eye.z);
        out.level[l][2] = lv.texel;
        out.level[l][3] = static_cast<float>(lv.layer);
        any = any || lv.layer >= 0;
    }
    out.params[0] = any && landSunImage_.isValid() ? 1.0f : 0.0f;
    out.params[1] = eye.y;
    if (landSunRadius_ > 0.0) {
        out.body[0] = eye.x;
        out.body[1] = eye.z;
        out.body[2] = static_cast<float>(0.5 / landSunRadius_);
    }
}

VkImageView SceneMeshDrawer::landSunView() const {
    return landSunImage_.isValid() ? landSunImage_.view : landSunNone_.view;
}

void SceneMeshDrawer::offerLandSun(SceneFrame& frame, WorldService& service) {
    const bool tiles = landSunTiles_;
    landSunTiles_ = false;
    // From the first land offered with tiles to read (as its last fills
    // left them), under a sun.
    SceneNode* n = landOffered_.empty() ? nullptr : frame.graph.findById(landOffered_.front());
    LandNode* land = n && n->type() == SceneNode::Type::Mesh ? static_cast<MeshNode*>(n)->asLand() : nullptr;
    const float* d = frame.lighting.sunDirection;
    if (!land || land->stats().resident == 0 || d[3] < 0.5f) {
        landSun_.forget();
        return;
    }
    landSunRadius_ = land->round() ? land->place()->body().radius : 0.0;
    // What a level reads of the land: its module and rings, and its canopy
    // (where, and how far from the eye it rises).
    uint64_t generation = land->generation();
    generation = mixBits(generation, land->canopySourceHash());
    generation = mixBits(generation, floatBits(land->canopyCastRange()));
    generation = mixBits(generation, land->id());
    // (The nearest ground no nearer than the eye's height over the land's
    // highest: a metre of it spans the focal length over that, at most.)
    const SceneView& view = frame.world;
    const float focal = 0.5f * static_cast<float>(std::max(view.height, 1u)) * std::fabs(view.proj.at(1, 1));
    const float over = std::max(view.eye.y - land->chartBounds().max.y, 1.0f);
    landSun_.update(view.eye, {-d[0], -d[1], -d[2]}, generation, tiles, focal / over);
    for (const LandSun::Want& w : landSun_.wants()) {
        const double side = static_cast<double>(LS_SIDE) * LandSun::kTexel0 * std::ldexp(1.0, w.level);
        service.offer({static_cast<uint64_t>(w.level), land->id(), landSunKind_, w.error, side, w.required});
    }
}

uint32_t SceneMeshDrawer::recordLandSun(SceneFrame& frame, std::span<const WorldJob> granted, WorldStream& stream) {
    if (granted.empty()) return 0;
    const auto found = lands_.find(granted.front().producer);
    if (found == lands_.end() || found->second->failed) return 0;
    const LandTiles& lt = *found->second;
    SceneNode* n = frame.graph.findById(found->first);
    LandNode* land = n && n->type() == SceneNode::Type::Mesh ? static_cast<MeshNode*>(n)->asLand() : nullptr;
    if (!land || land->stats().resident == 0) return 0;
    // Written and sampled in GENERAL, where it stays.
    if (!landSunImage_.isValid() &&
        !frame.gpu.allocator.createStorageImage(LS_SIDE, LS_SIDE, 1, kLandSunFormat, landSunImage_, false, LS_LAYERS)) {
        LOG_ERROR("SceneMeshDrawer: failed creating the land's sun's levels");
        return 0;
    }

    float words[LD_WORDS];
    landWords(frame, *land, lt, words);
    const VkDescriptorBufferInfo ubo = frame.gpu.device.frameUniform(words, sizeof(words));
    VkDescriptorSet set = frame.gpu.device.frameSet(landSunSetLayout_);
    if (set == VK_NULL_HANDLE) return 0;
    SceneVkDescriptorWriter writer;
    writer.writeBuffer(0, ubo.buffer, ubo.range, ubo.offset);
    writer.writeImage(1, lt.atlas.layerView(0), lt.atlas.layerSampler(), VK_IMAGE_LAYOUT_GENERAL);
    writer.writeImage(2, lt.atlas.layerView(1), lt.atlas.layerSampler(), VK_IMAGE_LAYOUT_GENERAL);
    writer.writeImage(3, lt.atlas.pageView(), lt.atlas.pageSampler());
    writer.writeImage(4, lt.atlas.layerView(3), lt.atlas.layerSampler(), VK_IMAGE_LAYOUT_GENERAL);
    writer.writeImage(5, landSunImage_.view, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_GENERAL, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE);
    writer.updateSet(device_, set);

    // The highest anything shading a level stands: the land's top (its
    // canopy's included).
    const float ceiling = land->highest();
    const float* d = frame.lighting.sunDirection;
    VkCommandBuffer cmd = stream.cmd();
    // The tiles and page entries this frame's fills wrote.
    VkMemoryBarrier mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    mb.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
    mb.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &mb, 0, nullptr, 0, nullptr);
    bindScenePipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, landSunPipeline_);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, landSunLayout_, 0, 1, &set, 0, nullptr);
    uint32_t recorded = 0;
    for (const WorldJob& job : granted) {
        const LandSun::Level lv = landSun_.make(static_cast<int>(job.key));
        // The copy made: what earlier frames read of it is done with.
        VkMemoryBarrier to{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
        to.srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
        to.dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1,
                             &to, 0, nullptr, 0, nullptr);
        const float p[LS_P_WORDS] = {static_cast<float>(lv.x0), static_cast<float>(lv.z0), lv.texel,
                                     static_cast<float>(lv.layer), -d[0], -d[1], -d[2], ceiling};
        vkCmdPushConstants(cmd, landSunLayout_, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(p), p);
        constexpr uint32_t groups = (LS_SIDE + LS_GROUP - 1) / LS_GROUP;
        vkCmdDispatch(cmd, groups, groups, 1);
        // Shown from here on: every lit fragment after reads it.
        VkMemoryBarrier back{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
        back.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        back.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 1,
                             &back, 0, nullptr, 0, nullptr);
        ++recorded;
    }
    return recorded;
}

}  // namespace bro::scene::vk
