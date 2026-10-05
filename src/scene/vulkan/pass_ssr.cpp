#include "scene/vulkan/pass_ssr.h"

#include "scene/scene_renderer.h"
#include "scene/vulkan/scene_defaults.h"
#include "scene/vulkan/scene_frame.h"
#include "scene/vulkan/scene_targets.h"
#include "scene/vulkan/scene_vk_device.h"
#include "scene/vulkan/scene_vk_pipeline.h"
#include "scene/vulkan/scene_vk_shader_compiler.h"

#include <cstring>

namespace bro::scene::vk {

bool PassSSR::setup(SceneGpu& gpu) {
    device_ = &gpu.device;
    VkDevice dev = gpu.device.device();
    SceneVkDescriptorLayoutBuilder builder;
    builder.addBinding(0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT);
    builder.addBinding(1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT);
    builder.addBinding(2, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT);
    setLayout_ = builder.build(dev);
    if (!setLayout_) return false;

    VkPipelineLayoutCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    info.setLayoutCount = 1;
    info.pSetLayouts = &setLayout_;
    if (vkCreatePipelineLayout(dev, &info, nullptr, &layout_) != VK_SUCCESS) return false;

    vs_ = SceneVkShaderCompiler::createBuiltinModule(dev, BuiltinSceneShader::PostFxVert);
    fs_ = SceneVkShaderCompiler::createBuiltinModule(dev, BuiltinSceneShader::SsrFrag);
    return vs_ && fs_;
}

void PassSSR::cleanup(SceneGpu& gpu) {
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

bool PassSSR::active(const SceneFrame& frame) const {
    return frame.ssr;
}

void PassSSR::declare(const SceneFrame& frame, PassIO& io) const {
    io.sample(frame.gpu.targets.ssrSnapshot);
    io.sample(frame.gpu.targets.depthSnapshot);
    io.hdr();
}

void PassSSR::record(SceneFrame& frame) {
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

    const SceneRenderer& r = frame.renderer;
    Uniforms u{};
    std::memcpy(u.proj, frame.view.proj.data, sizeof(u.proj));
    std::memcpy(u.invProj, frame.view.invProj.data, sizeof(u.invProj));
    u.params1[0] = frame.view.perspective ? 1.0f : 0.0f;
    u.params1[1] = r.ssrMaxDistance();
    u.params1[2] = static_cast<float>(r.ssrSteps());
    u.params1[3] = r.ssrThickness();
    u.params2[0] = r.ssrIntensity();
    u.params2[1] = r.ssrEdgeFade();
    const VkDescriptorBufferInfo ubo = device_->frameUniform(&u, sizeof(u));

    const SceneTargets& t = frame.gpu.targets;
    VkDescriptorSet set = device_->frameSet(setLayout_);
    SceneVkDescriptorWriter writer;
    writer.writeImage(0, t.ssrSnapshot.view, frame.gpu.defaults.sampler);
    writer.writeImage(1, t.depthSnapshot.view, t.depthSnapshot.sampler);   // nearest: depths never blend
    writer.writeBuffer(2, ubo.buffer, ubo.range, ubo.offset);
    writer.updateSet(device_->device(), set);

    vkCmdBindPipeline(frame.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
    vkCmdBindDescriptorSets(frame.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout_, 0, 1, &set, 0, nullptr);
    vkCmdDraw(frame.cmd, 3, 1, 0, 0);
}

}  // namespace bro::scene::vk
