#include "scene/vulkan/pass_postfx.h"

#include "scene/vulkan/scene_frame.h"
#include "scene/vulkan/scene_frame_graph.h"
#include "scene/vulkan/scene_fullscreen.h"
#include "scene/vulkan/scene_targets.h"
#include "scene/vulkan/scene_vk_device.h"
#include "scene/vulkan/scene_vk_shader_compiler.h"
#include "util/log.h"

#include <algorithm>

namespace bro::scene::vk {

namespace {
constexpr float kBloomKnee = 0.5f;
}

bool PassPostFx::setup(SceneGpu& gpu) {
    device_ = &gpu.device;
    VkDevice dev = gpu.device.device();
    VkSamplerCreateInfo sampler{};
    sampler.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    sampler.magFilter = VK_FILTER_LINEAR;
    sampler.minFilter = VK_FILTER_LINEAR;
    sampler.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    sampler.addressModeU = sampler.addressModeV = sampler.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sampler.maxLod = 1.0f;
    if (vkCreateSampler(dev, &sampler, nullptr, &sampler_) != VK_SUCCESS) return false;

    singleSetLayout_ = fullscreen::samplerSetLayout(dev, 1);
    tonemapSetLayout_ = fullscreen::samplerSetLayout(dev, 2);   // HDR, bloom
    if (!singleSetLayout_ || !tonemapSetLayout_) return false;
    singleLayout_ = fullscreen::layout(dev, singleSetLayout_, sizeof(BloomPush));
    tonemapLayout_ = fullscreen::layout(dev, tonemapSetLayout_, sizeof(TonemapPush));

    VkShaderModule vs = SceneVkShaderCompiler::createBuiltinModule(dev, BuiltinSceneShader::PostFxVert);
    VkShaderModule bloomFs = SceneVkShaderCompiler::createBuiltinModule(dev, BuiltinSceneShader::BloomFrag);
    VkShaderModule tonemapFs = SceneVkShaderCompiler::createBuiltinModule(dev, BuiltinSceneShader::TonemapFrag);
    VkShaderModule fxaaFs = SceneVkShaderCompiler::createBuiltinModule(dev, BuiltinSceneShader::FxaaFrag);
    bloomPipeline_ = fullscreen::pipeline(dev, singleLayout_, vs, bloomFs, SceneTargets::kHdrFormat);
    tonemapPipeline_ = fullscreen::pipeline(dev, tonemapLayout_, vs, tonemapFs, SceneTargets::kLdrFormat);
    fxaaPipeline_ = fullscreen::pipeline(dev, singleLayout_, vs, fxaaFs, SceneTargets::kLdrFormat);
    for (VkShaderModule m : {vs, bloomFs, tonemapFs, fxaaFs}) SceneVkShaderModule::destroy(dev, m);
    return bloomPipeline_ && tonemapPipeline_ && fxaaPipeline_;
}

void PassPostFx::resize(SceneGpu& gpu, uint32_t width, uint32_t height) {
    SceneVkAllocator& a = gpu.allocator;
    for (SceneVkImage* img : {&bloomExtract_, &bloomBlur_, &preFxaa_}) a.destroyImage(*img);
    const VkImageUsageFlags usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    const uint32_t hw = std::max(1u, width / 2);
    const uint32_t hh = std::max(1u, height / 2);
    const VkMemoryPropertyFlags local = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
    if (!a.createImage(hw, hh, SceneTargets::kHdrFormat, usage, local, bloomExtract_) ||
        !a.createImage(hw, hh, SceneTargets::kHdrFormat, usage, local, bloomBlur_) ||
        !a.createImage(width, height, SceneTargets::kLdrFormat, usage, local, preFxaa_)) {
        LOG_ERROR("PassPostFx: Failed creating the %ux%u intermediate targets", width, height);
    }
}

void PassPostFx::cleanup(SceneGpu& gpu) {
    VkDevice dev = gpu.device.device();
    for (SceneVkImage* img : {&bloomExtract_, &bloomBlur_, &preFxaa_}) gpu.allocator.destroyImage(*img);
    gpu.device.defer([dev, s = sampler_, p0 = bloomPipeline_, p1 = tonemapPipeline_, p2 = fxaaPipeline_] {
        if (s) vkDestroySampler(dev, s, nullptr);
        for (VkPipeline p : {p0, p1, p2}) {
            if (p) vkDestroyPipeline(dev, p, nullptr);
        }
    });
    sampler_ = VK_NULL_HANDLE;
    bloomPipeline_ = tonemapPipeline_ = fxaaPipeline_ = VK_NULL_HANDLE;
    for (VkPipelineLayout* l : {&singleLayout_, &tonemapLayout_}) {
        if (*l) vkDestroyPipelineLayout(dev, *l, nullptr);
        *l = VK_NULL_HANDLE;
    }
    for (VkDescriptorSetLayout* l : {&singleSetLayout_, &tonemapSetLayout_}) {
        if (*l) vkDestroyDescriptorSetLayout(dev, *l, nullptr);
        *l = VK_NULL_HANDLE;
    }
}

SceneVkImage& PassPostFx::source(const SceneFrame& frame) {
    return frame.dof ? frame.gpu.targets.dofHdr : frame.gpu.targets.hdr;
}

SceneVkImage& PassPostFx::output(const SceneFrame& frame) {
    return frame.lut ? frame.gpu.targets.postLdr : frame.gpu.targets.ldr;
}

void PassPostFx::declare(const SceneFrame& frame, PassIO& io) const {
    io.sample(source(frame));
    io.colorTarget(output(frame));
}

void PassPostFx::pass(SceneFrame& frame, SceneVkImage& target, VkPipeline pipeline, VkPipelineLayout layout,
                      VkDescriptorSet set, const void* push, uint32_t pushBytes) {
    SceneFrameGraph::transition(frame.cmd, target, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    fullscreen::draw(*device_, frame.cmd, target, pipeline, layout, set, push, pushBytes);
    SceneFrameGraph::transition(frame.cmd, target, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
}

void PassPostFx::bloom(SceneFrame& frame, const SceneVkImage& hdr) {
    BloomPush push{};
    push.threshold = frame.renderer.bloomThreshold();
    push.knee = kBloomKnee;

    VkDescriptorSet extractSet = device_->frameSet(singleSetLayout_);
    SceneVkDescriptorWriter extract;
    extract.writeImage(0, hdr.view, sampler_);
    extract.updateSet(device_->device(), extractSet);
    pass(frame, bloomExtract_, bloomPipeline_, singleLayout_, extractSet, &push, sizeof(push));

    VkDescriptorSet blurSet = device_->frameSet(singleSetLayout_);
    SceneVkDescriptorWriter blur;
    blur.writeImage(0, bloomExtract_.view, sampler_);
    blur.updateSet(device_->device(), blurSet);
    push.blurRadius = 1.0f / static_cast<float>(bloomBlur_.width);
    push.passType = 1;
    pass(frame, bloomBlur_, bloomPipeline_, singleLayout_, blurSet, &push, sizeof(push));
}

void PassPostFx::record(SceneFrame& frame) {
    const SceneRenderer& r = frame.renderer;
    const SceneVkImage& hdr = source(frame);
    SceneVkImage& out = output(frame);
    const bool bloomOn = r.bloomEnabled() && r.bloomIntensity() > 0.0f && bloomBlur_.isValid();
    const bool fxaa = r.fxaaEnabled() && preFxaa_.isValid();
    if (bloomOn) bloom(frame, hdr);

    VkDescriptorSet tonemapSet = device_->frameSet(tonemapSetLayout_);
    SceneVkDescriptorWriter writer;
    writer.writeImage(0, hdr.view, sampler_);
    writer.writeImage(1, bloomOn ? bloomBlur_.view : hdr.view, sampler_);
    writer.updateSet(device_->device(), tonemapSet);
    TonemapPush tonemap{};
    tonemap.exposure = r.exposure();
    tonemap.gamma = r.gamma();
    tonemap.bloomIntensity = bloomOn ? r.bloomIntensity() : 0.0f;
    tonemap.tonemapMode = static_cast<int32_t>(r.toneMap());
    tonemap.enableFxaa = fxaa ? 1 : 0;
    tonemap.texelSize[0] = 1.0f / static_cast<float>(out.width);
    tonemap.texelSize[1] = 1.0f / static_cast<float>(out.height);
    if (!fxaa) {
        fullscreen::draw(*device_, frame.cmd, out, tonemapPipeline_, tonemapLayout_, tonemapSet, &tonemap,
                         sizeof(tonemap));
        return;
    }
    pass(frame, preFxaa_, tonemapPipeline_, tonemapLayout_, tonemapSet, &tonemap, sizeof(tonemap));

    VkDescriptorSet fxaaSet = device_->frameSet(singleSetLayout_);
    SceneVkDescriptorWriter fxaaWriter;
    fxaaWriter.writeImage(0, preFxaa_.view, sampler_);
    fxaaWriter.updateSet(device_->device(), fxaaSet);
    const float texel[4] = {tonemap.texelSize[0], tonemap.texelSize[1], 0.0f, 0.0f};
    fullscreen::draw(*device_, frame.cmd, out, fxaaPipeline_, singleLayout_, fxaaSet, texel, sizeof(texel));
}

}  // namespace bro::scene::vk
