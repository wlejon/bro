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
#include "util/log.h"

#include <algorithm>
#include <cstdlib>

namespace bro::scene::vk {

bool hiZEnabled() {
    static const bool on = [] {
        const char* env = std::getenv("BRO_SCENE_HIZ");
        return !(env && *env == '0');
    }();
    return on;
}

bool PassHiZ::setup(SceneGpu& gpu) {
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
    for (VkImageView v : levelViews_) vkDestroyImageView(device_, v, nullptr);
    levelViews_.clear();
    viewsOf_ = VK_NULL_HANDLE;
}

void PassHiZ::resize(SceneGpu& gpu, uint32_t, uint32_t) {
    // A storage view a level (the targets were just made: no frame reads
    // the old ones any more, the resize having waited for them).
    gpu.device.frames().defer([device = device_, views = levelViews_] {
        for (VkImageView v : views) vkDestroyImageView(device, v, nullptr);
    });
    levelViews_.clear();
    viewsOf_ = VK_NULL_HANDLE;
    const SceneVkImage& hiZ = gpu.targets.hiZ;
    if (!hiZ.isValid()) return;
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
        levelViews_.push_back(view);
    }
    viewsOf_ = hiZ.image;
}

bool PassHiZ::active(const SceneFrame& frame) const {
    return hiZEnabled() && pipeline_ && !batches_.draws[batch_].empty() && frame.gpu.targets.hiZ.isValid() &&
           viewsOf_ == frame.gpu.targets.hiZ.image;
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
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline_);
    for (uint32_t level = 0; level < levelViews_.size(); ++level) {
        if (level > 0) {
            // The level below written before this one reads it.
            render::cmdMemoryBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT,
                                     VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT);
        }
        VkDescriptorSet set = frame.gpu.device.frameSet(setLayout_);
        if (set == VK_NULL_HANDLE) return;
        SceneVkDescriptorWriter writer;
        writer.writeImage(0, t.farDepth.view, sampler_);
        writer.writeImage(1, levelViews_[level > 0 ? level - 1 : 0], VK_NULL_HANDLE, VK_IMAGE_LAYOUT_GENERAL,
                          VK_DESCRIPTOR_TYPE_STORAGE_IMAGE);
        writer.writeImage(2, levelViews_[level], VK_NULL_HANDLE, VK_IMAGE_LAYOUT_GENERAL,
                          VK_DESCRIPTOR_TYPE_STORAGE_IMAGE);
        writer.updateSet(device_, set);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, layout_, 0, 1, &set, 0, nullptr);
        const int32_t fromFarDepth = level == 0 ? 1 : 0;
        vkCmdPushConstants(cmd, layout_, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(fromFarDepth), &fromFarDepth);
        const uint32_t w = std::max(t.hiZ.width >> level, 1u), h = std::max(t.hiZ.height >> level, 1u);
        vkCmdDispatch(cmd, (w + 7) / 8, (h + 7) / 8, 1);
    }
}

void PassHiZ::cleanup(SceneGpu&) {
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
