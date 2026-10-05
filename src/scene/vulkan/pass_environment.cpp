#include "scene/vulkan/pass_environment.h"

#include "scene/scene_renderer.h"
#include "scene/vulkan/scene_defaults.h"
#include "scene/vulkan/scene_frame.h"
#include "scene/vulkan/scene_vk_device.h"
#include "scene/vulkan/scene_vk_pipeline.h"
#include "scene/vulkan/scene_vk_shader_compiler.h"

#include <cstring>

namespace bro::scene::vk {

namespace {

constexpr float kSkyColor[3] = {0.2f, 0.4f, 0.8f};
constexpr float kHorizonColor[3] = {0.7f, 0.75f, 0.8f};
constexpr float kGroundColor[3] = {0.2f, 0.18f, 0.15f};

}  // namespace

bool PassEnvironment::setup(SceneGpu& gpu) {
    device_ = &gpu.device;
    VkDevice dev = gpu.device.device();

    SceneVkDescriptorLayoutBuilder builder;
    builder.addBinding(0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT);
    cubeLayout_ = builder.build(dev);
    if (!cubeLayout_) return false;

    VkPushConstantRange push{VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(Push)};
    VkPipelineLayoutCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    info.setLayoutCount = 1;
    info.pSetLayouts = &cubeLayout_;
    info.pushConstantRangeCount = 1;
    info.pPushConstantRanges = &push;
    if (vkCreatePipelineLayout(dev, &info, nullptr, &layout_) != VK_SUCCESS) return false;

    vs_ = SceneVkShaderCompiler::createBuiltinModule(dev, BuiltinSceneShader::SkyboxVert);
    fs_ = SceneVkShaderCompiler::createBuiltinModule(dev, BuiltinSceneShader::EnvironmentFrag);
    return vs_ && fs_;
}

void PassEnvironment::cleanup(SceneGpu& gpu) {
    VkDevice dev = gpu.device.device();
    pipelines_.destroy(gpu.device);
    SceneVkShaderModule::destroy(dev, vs_);
    SceneVkShaderModule::destroy(dev, fs_);
    vs_ = fs_ = VK_NULL_HANDLE;
    if (layout_) vkDestroyPipelineLayout(dev, layout_, nullptr);
    if (cubeLayout_) vkDestroyDescriptorSetLayout(dev, cubeLayout_, nullptr);
    layout_ = VK_NULL_HANDLE;
    cubeLayout_ = VK_NULL_HANDLE;
}

bool PassEnvironment::active(const SceneFrame& frame) const {
    return frame.renderer.atmosphere().enabled || !frame.renderer.environmentPath().empty();
}

void PassEnvironment::declare(const SceneFrame& frame, PassIO& io) const {
    io.hdr({.indirect = frame.ssao});
}

void PassEnvironment::record(SceneFrame& frame) {
    VkPipeline pipeline = pipelines_.get(0, frame.hdrTarget, [&] {
        SceneVkPipelineBuilder b;
        b.setShaderStages(vs_, fs_)
         .setInputTopology(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST)
         .setPolygonMode(VK_POLYGON_MODE_FILL)
         .setCullMode(VK_CULL_MODE_NONE)
         .setTarget(frame.hdrTarget)
         .disableBlending(frame.hdrTarget.colorCount)
         // At the far plane: passes where nothing has been drawn, writes nothing.
         .enableDepthTest(false);
        return b.build(device_->device(), layout_);
    });
    if (!pipeline) return;

    const SceneDefaults& d = frame.gpu.defaults;
    VkDescriptorSet set = device_->frameSet(cubeLayout_);
    SceneVkDescriptorWriter writer;
    writer.writeImage(0, d.cube.view, d.cubeSampler);
    writer.updateSet(device_->device(), set);

    Push push{};
    std::memcpy(push.invViewProj, frame.view.invViewProj.data, sizeof(push.invViewProj));
    push.sunDir[0] = frame.lighting.sunDirection[0];
    push.sunDir[1] = frame.lighting.sunDirection[1];
    push.sunDir[2] = frame.lighting.sunDirection[2];
    push.sunDir[3] = 0.0f;
    std::memcpy(push.skyColor, kSkyColor, sizeof(kSkyColor));
    push.skyColor[3] = frame.renderer.environmentIntensity();
    std::memcpy(push.horizonColor, kHorizonColor, sizeof(kHorizonColor));
    push.horizonColor[3] = 1.0f;
    std::memcpy(push.groundColor, kGroundColor, sizeof(kGroundColor));
    push.groundColor[3] = frame.lighting.sunColor[3];

    VkCommandBuffer cmd = frame.cmd;
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout_, 0, 1, &set, 0, nullptr);
    vkCmdPushConstants(cmd, layout_, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(push), &push);
    vkCmdDraw(cmd, 3, 1, 0, 0);
}

}  // namespace bro::scene::vk
