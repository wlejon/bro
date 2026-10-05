#include "scene/vulkan/pass_dof.h"

#include "scene/vulkan/scene_frame.h"
#include "scene/vulkan/scene_frame_graph.h"
#include "scene/vulkan/scene_fullscreen.h"
#include "scene/vulkan/scene_targets.h"
#include "scene/vulkan/scene_vk_device.h"
#include "scene/vulkan/scene_vk_shader_compiler.h"
#include "util/log.h"

#include <algorithm>

namespace bro::scene::vk {

bool PassDoF::setup(SceneGpu& gpu) {
    device_ = &gpu.device;
    VkDevice dev = gpu.device.device();
    VkSamplerCreateInfo sampler{};
    sampler.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    sampler.magFilter = VK_FILTER_LINEAR;
    sampler.minFilter = VK_FILTER_LINEAR;
    sampler.addressModeU = sampler.addressModeV = sampler.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    if (vkCreateSampler(dev, &sampler, nullptr, &sampler_) != VK_SUCCESS) return false;

    blurSetLayout_ = fullscreen::samplerSetLayout(dev, 1);
    compositeSetLayout_ = fullscreen::samplerSetLayout(dev, 3);
    if (!blurSetLayout_ || !compositeSetLayout_) return false;
    blurLayout_ = fullscreen::layout(dev, blurSetLayout_, 4 * sizeof(float));
    compositeLayout_ = fullscreen::layout(dev, compositeSetLayout_, 5 * sizeof(float));

    VkShaderModule vs = SceneVkShaderCompiler::createBuiltinModule(dev, BuiltinSceneShader::PostFxVert);
    VkShaderModule blurFs = SceneVkShaderCompiler::createBuiltinModule(dev, BuiltinSceneShader::BlurFrag);
    VkShaderModule dofFs = SceneVkShaderCompiler::createBuiltinModule(dev, BuiltinSceneShader::DofFrag);
    blurPipeline_ = fullscreen::pipeline(dev, blurLayout_, vs, blurFs, SceneTargets::kHdrFormat);
    compositePipeline_ = fullscreen::pipeline(dev, compositeLayout_, vs, dofFs, SceneTargets::kHdrFormat);
    for (VkShaderModule m : {vs, blurFs, dofFs}) SceneVkShaderModule::destroy(dev, m);
    return blurPipeline_ && compositePipeline_;
}

void PassDoF::resize(SceneGpu& gpu, uint32_t width, uint32_t height) {
    for (SceneVkImage& img : blur_) gpu.allocator.destroyImage(img);
    const uint32_t w = std::max(1u, width / 2);
    const uint32_t h = std::max(1u, height / 2);
    for (SceneVkImage& img : blur_) {
        if (!gpu.allocator.createImage(w, h, SceneTargets::kHdrFormat,
                                       VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                                           VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                                       VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, img)) {
            LOG_ERROR("PassDoF: Failed creating the %ux%u blur targets", w, h);
        }
    }
}

void PassDoF::cleanup(SceneGpu& gpu) {
    VkDevice dev = gpu.device.device();
    for (SceneVkImage& img : blur_) gpu.allocator.destroyImage(img);
    gpu.device.defer([dev, s = sampler_, p0 = blurPipeline_, p1 = compositePipeline_] {
        if (s) vkDestroySampler(dev, s, nullptr);
        if (p0) vkDestroyPipeline(dev, p0, nullptr);
        if (p1) vkDestroyPipeline(dev, p1, nullptr);
    });
    sampler_ = VK_NULL_HANDLE;
    blurPipeline_ = compositePipeline_ = VK_NULL_HANDLE;
    for (VkPipelineLayout* l : {&blurLayout_, &compositeLayout_}) {
        if (*l) vkDestroyPipelineLayout(dev, *l, nullptr);
        *l = VK_NULL_HANDLE;
    }
    for (VkDescriptorSetLayout* l : {&blurSetLayout_, &compositeSetLayout_}) {
        if (*l) vkDestroyDescriptorSetLayout(dev, *l, nullptr);
        *l = VK_NULL_HANDLE;
    }
}

bool PassDoF::active(const SceneFrame& frame) const {
    return frame.dof && blur_[0].isValid() && blur_[1].isValid();
}

void PassDoF::declare(const SceneFrame& frame, PassIO& io) const {
    SceneTargets& t = frame.gpu.targets;
    io.transferSrc(t.hdr);   // downsampled by blit, then sampled sharp
    io.sample(t.depthSnapshot);
    io.colorTarget(t.dofHdr);
}

void PassDoF::record(SceneFrame& frame) {
    VkCommandBuffer cmd = frame.cmd;
    SceneTargets& t = frame.gpu.targets;
    const float maxBlur = frame.renderer.depthOfFieldMaxBlur();

    // hdr -> blur_[0] at half resolution.
    SceneFrameGraph::transition(cmd, blur_[0], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkImageBlit blit{};
    blit.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    blit.srcOffsets[1] = {static_cast<int32_t>(t.hdr.width), static_cast<int32_t>(t.hdr.height), 1};
    blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    blit.dstOffsets[1] = {static_cast<int32_t>(blur_[0].width), static_cast<int32_t>(blur_[0].height), 1};
    vkCmdBlitImage(cmd, t.hdr.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, blur_[0].image,
                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_LINEAR);
    SceneFrameGraph::transition(cmd, t.hdr, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    SceneFrameGraph::transition(cmd, blur_[0], VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

    // Blur across into blur_[1], then down back into blur_[0].
    const float across[4] = {maxBlur / static_cast<float>(blur_[0].width), 0.0f, 0.0f, 0.0f};
    const float down[4] = {0.0f, maxBlur / static_cast<float>(blur_[0].height), 0.0f, 0.0f};
    for (int pass = 0; pass < 2; ++pass) {
        SceneVkImage& src = blur_[pass];
        SceneVkImage& dst = blur_[1 - pass];
        VkDescriptorSet set = device_->frameSet(blurSetLayout_);
        SceneVkDescriptorWriter writer;
        writer.writeImage(0, src.view, sampler_);
        writer.updateSet(device_->device(), set);
        SceneFrameGraph::transition(cmd, dst, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
        fullscreen::draw(*device_, cmd, dst, blurPipeline_, blurLayout_, set, pass == 0 ? across : down,
                         4 * sizeof(float));
        SceneFrameGraph::transition(cmd, dst, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    }

    // Sharp + blurred by circle of confusion -> dofHdr.
    VkDescriptorSet set = device_->frameSet(compositeSetLayout_);
    SceneVkDescriptorWriter writer;
    writer.writeImage(0, t.hdr.view, sampler_);
    writer.writeImage(1, blur_[0].view, sampler_);
    writer.writeImage(2, t.depthSnapshot.view, sampler_);
    writer.updateSet(device_->device(), set);
    const float push[5] = {frame.renderer.depthOfFieldFocusDistance(), frame.renderer.depthOfFieldFocusRange(),
                           frame.view.nearZ, frame.view.farZ, frame.view.perspective ? 1.0f : 0.0f};
    fullscreen::draw(*device_, cmd, t.dofHdr, compositePipeline_, compositeLayout_, set, push, sizeof(push));
}

}  // namespace bro::scene::vk
