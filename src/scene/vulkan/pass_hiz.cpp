#include "scene/vulkan/pass_hiz.h"

#include "render/vulkan_frames.h"
#include "render/vulkan_pipeline_cache.h"
#include "render/vulkan_util.h"
#include "scene/vulkan/pass_mesh.h"
#include "scene/vulkan/scene_frame.h"
#include "scene/vulkan/scene_targets.h"
#include "scene/vulkan/scene_vk_depth.h"
#include "scene/vulkan/scene_vk_descriptors.h"
#include "scene/vulkan/scene_vk_device.h"
#include "scene/vulkan/scene_vk_shader_compiler.h"
#include "scene/vulkan/shaders/occlusion_list_layout.h"
#include "scene/vulkan/shaders/scene_forest_layout.h"
#include "util/log.h"

#include <algorithm>
#include <vector>

namespace bro::scene::vk {

static_assert(sizeof(OcclusionListPipeline::OlDraws) == 16 * OL_SLOTS);

bool occlusionRetests(const SceneFrame& frame, const OpaqueBatches& batches) {
    // (As writeCameraSet's occlusion.z says the batches test the last
    // frame's pyramid; a pyramid is built when a slot draws.)
    return batches.slots > 0 && frame.gpu.targets.hiZPrevState.built;
}

bool OcclusionListPipeline::setup(SceneGpu& gpu) {
    device = gpu.device.device();
    VkSamplerCreateInfo si{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    si.magFilter = si.minFilter = VK_FILTER_NEAREST;
    si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    si.maxLod = VK_LOD_CLAMP_NONE;
    if (vkCreateSampler(device, &si, nullptr, &sampler) != VK_SUCCESS) return false;
    SceneVkDescriptorLayoutBuilder builder;
    builder.addBinding(0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT);
    builder.addBinding(1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT);
    builder.addBinding(2, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT);
    for (uint32_t i = 0; i < OL_PLANNED; ++i)
        builder.addBinding(3 + i, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT);
    setLayout = builder.build(device);
    if (!setLayout) return false;
    VkPushConstantRange push{VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(int32_t)};
    VkPipelineLayoutCreateInfo pi{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pi.setLayoutCount = 1;
    pi.pSetLayouts = &setLayout;
    pi.pushConstantRangeCount = 1;
    pi.pPushConstantRanges = &push;
    if (vkCreatePipelineLayout(device, &pi, nullptr, &layout) != VK_SUCCESS) return false;
    VkShaderModule module = SceneVkShaderCompiler::createBuiltinModule(device, BuiltinSceneShader::OcclusionListComp);
    if (!module) return false;
    const VkBool32 reversed = reversedZ() ? VK_TRUE : VK_FALSE;
    const VkSpecializationMapEntry entry{depth::kReversedZConstantId, 0, sizeof(VkBool32)};
    VkSpecializationInfo spec{1, &entry, sizeof(reversed), &reversed};
    VkComputePipelineCreateInfo ci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    ci.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    ci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    ci.stage.module = module;
    ci.stage.pName = "main";
    ci.stage.pSpecializationInfo = &spec;
    ci.layout = layout;
    const bool ok = vkCreateComputePipelines(device, gpu.device.context().pipelineCache(), 1, &ci, nullptr,
                                             &pipeline) == VK_SUCCESS;
    SceneVkShaderModule::destroy(device, module);
    if (!ok) {
        pipeline = VK_NULL_HANDLE;
        LOG_ERROR("OcclusionListPipeline: failed creating the list's pipeline");
    }
    return ok;
}

void OcclusionListPipeline::cleanup() {
    if (pipeline) vkDestroyPipeline(device, pipeline, nullptr);
    if (layout) vkDestroyPipelineLayout(device, layout, nullptr);
    if (setLayout) vkDestroyDescriptorSetLayout(device, setLayout, nullptr);
    if (sampler) vkDestroySampler(device, sampler, nullptr);
    pipeline = VK_NULL_HANDLE;
    layout = VK_NULL_HANDLE;
    setLayout = VK_NULL_HANDLE;
    sampler = VK_NULL_HANDLE;
}

void OcclusionListPipeline::dispatch(SceneFrame& frame, int stage, const SceneVkImage& hiZ,
                                     VkImageLayout imageLayout, const OlDraws* draws,
                                     const VkBuffer* planned) const {
    VkCommandBuffer cmd = frame.cmd;
    VkDescriptorSet set = frame.gpu.device.frameSet(setLayout);
    if (set == VK_NULL_HANDLE) return;
    const SceneVkBuffer& list = frame.gpu.targets.occlusionList;
    // (The stages after the budget read no draws: an empty block.)
    static const OlDraws kNone{};
    const VkDescriptorBufferInfo ubo = frame.gpu.device.frameUniform(draws ? draws : &kNone, sizeof(OlDraws));
    SceneVkDescriptorWriter writer;
    writer.writeImage(0, hiZ.view, sampler, imageLayout);
    writer.writeBuffer(1, list.buffer, list.size, 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
    writer.writeBuffer(2, ubo.buffer, ubo.range, ubo.offset);
    for (uint32_t i = 0; i < OL_PLANNED; ++i) {
        const VkBuffer b = planned && planned[i] != VK_NULL_HANDLE ? planned[i] : list.buffer;
        writer.writeBuffer(3 + i, b, VK_WHOLE_SIZE, 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
    }
    writer.updateSet(device, set);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, layout, 0, 1, &set, 0, nullptr);
    const int32_t s = stage;
    vkCmdPushConstants(cmd, layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(s), &s);
    if (stage == OL_STAGE_TEST)
        vkCmdDispatchIndirect(cmd, list.buffer, static_cast<VkDeviceSize>(OL_DISPATCH) * 4);
    else
        vkCmdDispatch(cmd, 1, 1, 1);
}

bool PassOcclusionBudget::active(const SceneFrame& frame) const {
    return pipeline_.pipeline && occlusionRetests(frame, batches_);
}

void PassOcclusionBudget::declare(const SceneFrame& frame, PassIO& io) const {
    // (The pyramid only to bind: the budget reads none of it.)
    io.sample(frame.gpu.targets.hiZPrev);
    // The GPU planner's draws' commands.
    if (frame.computeApart()) io.readsCompute();
}

void PassOcclusionBudget::record(SceneFrame& frame) {
    VkCommandBuffer cmd = frame.cmd;
    // Each slot's draw: the CPU's counts, or where its planner wrote its
    // command (a slot of a fifth planner's buffer holds nothing back).
    OcclusionListPipeline::OlDraws draws;
    VkBuffer planned[OL_PLANNED] = {};
    for (auto& v : draws.v) v = {0, 0, OL_DRAW_NONE, 0};
    for (const auto& batch : batches_.draws)
        for (const OpaqueBatches::Draw& d : batch) {
            if (d.slot < 0) continue;
            const MeshDraw& draw = frame.lists.meshes[d.mesh];
            auto& v = draws.v[d.slot];
            if (draw.indirect != VK_NULL_HANDLE) {
                uint32_t i = 0;
                while (i < OL_PLANNED && planned[i] != VK_NULL_HANDLE && planned[i] != draw.indirect) ++i;
                if (i == OL_PLANNED) continue;
                planned[i] = draw.indirect;
                const VkDeviceSize at =
                    draw.indirectOffset +
                    (d.batch >= 0 ? static_cast<VkDeviceSize>(d.batch + 1) * draw.indirectBatchStride : 0);
                v = {0, 0, i, static_cast<uint32_t>(at / 4)};
                continue;
            }
            const bool crowns = static_cast<int>(draw.push.extra[2]) == FOREST_SPRITE_CROWNS;
            v = {d.split && crowns ? d.count * FOREST_CARD_VERTICES : draw.indexCount,
                 d.split && !crowns ? d.count : draw.instanceCount, OL_DRAW_CPU, 0};
        }
    // After the last frame's retest and test read the list, and the
    // planners wrote their commands.
    render::cmdMemoryBarrier(cmd,
                             VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT | VK_PIPELINE_STAGE_VERTEX_SHADER_BIT |
                                 VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                             VK_ACCESS_SHADER_WRITE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                             VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
    pipeline_.dispatch(frame, OL_STAGE_BUDGET, frame.gpu.targets.hiZPrev, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                       &draws, planned);
    // The batches' sprites read the slots enabled and append their cards.
    render::cmdMemoryBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT,
                             VK_PIPELINE_STAGE_VERTEX_SHADER_BIT,
                             VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
}

bool PassHiZ::setup(SceneGpu& gpu) {
    if (!list_.setup(gpu)) return false;
    device_ = gpu.device.device();
    VkSamplerCreateInfo si{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    si.magFilter = si.minFilter = VK_FILTER_NEAREST;
    si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    if (vkCreateSampler(device_, &si, nullptr, &sampler_) != VK_SUCCESS) return false;
    SceneVkDescriptorLayoutBuilder builder;
    builder.addBinding(0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT);
    builder.addBinding(1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT);
    builder.addBinding(2, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT);
    setLayout_ = builder.build(device_);
    if (!setLayout_) return false;
    VkPushConstantRange push{VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(int32_t)};
    VkPipelineLayoutCreateInfo pi{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pi.setLayoutCount = 1;
    pi.pSetLayouts = &setLayout_;
    pi.pushConstantRangeCount = 1;
    pi.pPushConstantRanges = &push;
    if (vkCreatePipelineLayout(device_, &pi, nullptr, &layout_) != VK_SUCCESS) return false;
    VkShaderModule module = SceneVkShaderCompiler::createBuiltinModule(device_, BuiltinSceneShader::HiZBuildComp);
    if (!module) return false;
    const VkBool32 reversed = reversedZ() ? VK_TRUE : VK_FALSE;
    const VkSpecializationMapEntry entry{depth::kReversedZConstantId, 0, sizeof(VkBool32)};
    VkSpecializationInfo spec{1, &entry, sizeof(reversed), &reversed};
    VkComputePipelineCreateInfo ci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    ci.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    ci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    ci.stage.module = module;
    ci.stage.pName = "main";
    ci.stage.pSpecializationInfo = &spec;
    ci.layout = layout_;
    const bool ok = vkCreateComputePipelines(device_, gpu.device.context().pipelineCache(), 1, &ci, nullptr,
                                             &pipeline_) == VK_SUCCESS;
    SceneVkShaderModule::destroy(device_, module);
    if (!ok) {
        pipeline_ = VK_NULL_HANDLE;
        LOG_ERROR("PassHiZ: failed creating the pyramid's pipeline");
    }
    return ok;
}

void PassHiZ::destroyViews() {
    for (int i = 0; i < 2; ++i) {
        for (VkImageView v : levelViews_[i]) vkDestroyImageView(device_, v, nullptr);
        levelViews_[i].clear();
        viewsOf_[i] = VK_NULL_HANDLE;
    }
}

const std::vector<VkImageView>* PassHiZ::viewsOf(VkImage image) const {
    for (int i = 0; i < 2; ++i)
        if (image != VK_NULL_HANDLE && viewsOf_[i] == image) return &levelViews_[i];
    return nullptr;
}

void PassHiZ::resize(SceneGpu& gpu, uint32_t, uint32_t) {
    // A storage view a level of each pyramid (the targets were just made:
    // no frame reads the old ones any more, the resize having waited for
    // them).
    gpu.device.frames().defer([device = device_, a = levelViews_[0], b = levelViews_[1]] {
        for (VkImageView v : a) vkDestroyImageView(device, v, nullptr);
        for (VkImageView v : b) vkDestroyImageView(device, v, nullptr);
    });
    for (int i = 0; i < 2; ++i) {
        levelViews_[i].clear();
        viewsOf_[i] = VK_NULL_HANDLE;
    }
    const SceneVkImage* images[2] = {&gpu.targets.hiZ, &gpu.targets.hiZPrev};
    for (int i = 0; i < 2; ++i) {
        const SceneVkImage& hiZ = *images[i];
        if (!hiZ.isValid()) continue;
        for (uint32_t level = 0; level < hiZ.mipLevels; ++level) {
            VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
            vi.image = hiZ.image;
            vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
            vi.format = hiZ.format;
            vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, level, 1, 0, 1};
            VkImageView view = VK_NULL_HANDLE;
            if (vkCreateImageView(device_, &vi, nullptr, &view) != VK_SUCCESS) {
                LOG_ERROR("PassHiZ: failed creating a level's view");
                destroyViews();
                return;
            }
            levelViews_[i].push_back(view);
        }
        viewsOf_[i] = hiZ.image;
    }
}

bool PassHiZ::active(const SceneFrame& frame) const {
    return pipeline_ &&batches_.slots > 0 && frame.gpu.targets.hiZ.isValid() &&
           viewsOf(frame.gpu.targets.hiZ.image);
}

void PassHiZ::declare(const SceneFrame& frame, PassIO& io) const {
    io.sample(frame.gpu.targets.farDepth);
    io.storage(frame.gpu.targets.hiZ);
    // (Nothing of the resolved HDR images: the batch's scope stays unresolved
    // to the next, as PassFarDepth's alone did.)
    io.unresolved();
}

void PassHiZ::record(SceneFrame& frame) {
    SceneTargets& t = frame.gpu.targets;
    VkCommandBuffer cmd = frame.cmd;
    const std::vector<VkImageView>& levelViews = *viewsOf(t.hiZ.image);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline_);
    for (uint32_t level = 0; level < levelViews.size(); ++level) {
        if (level > 0) {
            // The level below written before this one reads it.
            render::cmdMemoryBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT,
                                     VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT);
        }
        VkDescriptorSet set = frame.gpu.device.frameSet(setLayout_);
        if (set == VK_NULL_HANDLE) return;
        SceneVkDescriptorWriter writer;
        writer.writeImage(0, t.farDepth.view, sampler_);
        writer.writeImage(1, levelViews[level > 0 ? level - 1 : 0], VK_NULL_HANDLE, VK_IMAGE_LAYOUT_GENERAL,
                          VK_DESCRIPTOR_TYPE_STORAGE_IMAGE);
        writer.writeImage(2, levelViews[level], VK_NULL_HANDLE, VK_IMAGE_LAYOUT_GENERAL,
                          VK_DESCRIPTOR_TYPE_STORAGE_IMAGE);
        writer.updateSet(device_, set);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, layout_, 0, 1, &set, 0, nullptr);
        const int32_t fromFarDepth = level == 0 ? 1 : 0;
        vkCmdPushConstants(cmd, layout_, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(fromFarDepth), &fromFarDepth);
        const uint32_t w = std::max(t.hiZ.width >> level, 1u), h = std::max(t.hiZ.height >> level, 1u);
        vkCmdDispatch(cmd, (w + 7) / 8, (h + 7) / 8, 1);
    }
    // The view it is of, for the next frame to reproject into.
    t.hiZState.built = true;
    t.hiZState.proj = frame.view.proj;
    t.hiZState.relView = frame.view.relView;
    t.hiZState.eye = frame.view.eye;
    if (!occlusionRetests(frame, batches_)) return;

    // The cards the batches held back, against it: each slot's run of the
    // ones it does not hide, and the retest's command for them.
    render::cmdMemoryBarrier(cmd, VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                             VK_ACCESS_SHADER_WRITE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                             VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
    list_.dispatch(frame, OL_STAGE_PREFIX, t.hiZ, VK_IMAGE_LAYOUT_GENERAL);
    render::cmdMemoryBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT,
                             VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                             VK_ACCESS_INDIRECT_COMMAND_READ_BIT | VK_ACCESS_SHADER_READ_BIT |
                                 VK_ACCESS_SHADER_WRITE_BIT);
    list_.dispatch(frame, OL_STAGE_TEST, t.hiZ, VK_IMAGE_LAYOUT_GENERAL);
    render::cmdMemoryBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT,
                             VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT | VK_PIPELINE_STAGE_VERTEX_SHADER_BIT,
                             VK_ACCESS_INDIRECT_COMMAND_READ_BIT | VK_ACCESS_SHADER_READ_BIT);
}

void PassHiZ::cleanup(SceneGpu&) {
    list_.cleanup();
    destroyViews();
    if (pipeline_) vkDestroyPipeline(device_, pipeline_, nullptr);
    if (layout_) vkDestroyPipelineLayout(device_, layout_, nullptr);
    if (setLayout_) vkDestroyDescriptorSetLayout(device_, setLayout_, nullptr);
    if (sampler_) vkDestroySampler(device_, sampler_, nullptr);
    pipeline_ = VK_NULL_HANDLE;
    layout_ = VK_NULL_HANDLE;
    setLayout_ = VK_NULL_HANDLE;
    sampler_ = VK_NULL_HANDLE;
}

}  // namespace bro::scene::vk
