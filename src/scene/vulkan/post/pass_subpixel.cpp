#include "scene/vulkan/post/pass_subpixel.h"

#include "scene/vulkan/frame/scene_defaults.h"
#include "scene/vulkan/frame/scene_frame.h"
#include "scene/vulkan/frame/scene_fullscreen.h"
#include "scene/vulkan/frame/scene_targets.h"
#include "scene/vulkan/frame/scene_vk_descriptors.h"
#include "scene/vulkan/frame/scene_vk_device.h"
#include "scene/vulkan/frame/scene_vk_pipeline.h"
#include "scene/vulkan/frame/scene_vk_shader_compiler.h"

namespace bro::scene::vk {

bool PassSubpixel::setup(SceneGpu& gpu) {
    device_ = &gpu.device;
    VkDevice dev = gpu.device.device();
    SceneVkDescriptorLayoutBuilder builder;
    builder.addBinding(0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT);
    builder.addBinding(1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT);
    builder.addBinding(2, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT);
    setLayout_ = builder.build(dev);
    if (!setLayout_) return false;
    layout_ = fullscreen::layout(dev, setLayout_, 4 * sizeof(float));
    vs_ = SceneVkShaderCompiler::createBuiltinModule(dev, BuiltinSceneShader::PostFxVert);
    fs_ = SceneVkShaderCompiler::createBuiltinModule(dev, BuiltinSceneShader::SubpixelFrag);
    return layout_ && vs_ && fs_;
}

void PassSubpixel::cleanup(SceneGpu& gpu) {
    VkDevice dev = gpu.device.device();
    pipelines_.destroy(gpu.device);
    SceneVkShaderModule::destroy(dev, vs_);
    SceneVkShaderModule::destroy(dev, fs_);
    vs_ = fs_ = VK_NULL_HANDLE;
    if (layout_) vkDestroyPipelineLayout(dev, layout_, nullptr);
    if (setLayout_) vkDestroyDescriptorSetLayout(dev, setLayout_, nullptr);
    layout_ = VK_NULL_HANDLE;
    setLayout_ = VK_NULL_HANDLE;
}

bool PassSubpixel::active(const SceneFrame& frame) const {
    return frame.subpixel;
}

void PassSubpixel::declare(const SceneFrame& frame, PassIO& io) const {
    io.sample(frame.gpu.targets.colorSnapshot);
    io.sample(frame.gpu.targets.mark);
    io.sample(frame.gpu.targets.depthSnapshot);
    io.hdr();
}

void PassSubpixel::record(SceneFrame& frame) {
    VkPipeline pipeline = pipelines_.get(0, frame.hdrTarget, [&] {
        SceneVkPipelineBuilder b;
        b.setShaderStages(vs_, fs_)
         .setCullMode(VK_CULL_MODE_NONE)
         .setTarget(frame.hdrTarget)
         .disableBlending(1)
         .disableDepthTest();
        return b.build(device_->device(), layout_);
    });
    if (!pipeline) return;

    const SceneTargets& t = frame.gpu.targets;
    VkDescriptorSet set = device_->frameSet(setLayout_);
    SceneVkDescriptorWriter writer;
    writer.writeImage(0, t.colorSnapshot.view, frame.gpu.defaults.sampler);
    writer.writeImage(1, t.mark.view, t.mark.sampler);
    writer.writeImage(2, t.depthSnapshot.view, t.depthSnapshot.sampler);
    writer.updateSet(device_->device(), set);

    // A depth's distance from the eye: the inverse projection's z and w rows
    // (column-major), which no projection makes depend on x or y.
    const float* ip = frame.view.invProj.data;
    const float push[4] = {ip[10], ip[14], ip[11], ip[15]};
    vkCmdBindPipeline(frame.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
    vkCmdBindDescriptorSets(frame.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout_, 0, 1, &set, 0, nullptr);
    vkCmdPushConstants(frame.cmd, layout_, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(push), push);
    vkCmdDraw(frame.cmd, 3, 1, 0, 0);
}

}  // namespace bro::scene::vk
