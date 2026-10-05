#include "scene/vulkan/scene_fullscreen.h"

#include "scene/vulkan/scene_vk_descriptors.h"
#include "scene/vulkan/scene_vk_device.h"
#include "scene/vulkan/scene_vk_pipeline.h"
#include "scene/vulkan/scene_vk_target_format.h"

namespace bro::scene::vk::fullscreen {

VkPipelineLayout layout(VkDevice dev, VkDescriptorSetLayout set, uint32_t pushBytes) {
    VkPushConstantRange push{VK_SHADER_STAGE_FRAGMENT_BIT, 0, pushBytes};
    VkPipelineLayoutCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    info.setLayoutCount = 1;
    info.pSetLayouts = &set;
    info.pushConstantRangeCount = pushBytes ? 1 : 0;
    info.pPushConstantRanges = pushBytes ? &push : nullptr;
    VkPipelineLayout out = VK_NULL_HANDLE;
    if (vkCreatePipelineLayout(dev, &info, nullptr, &out) != VK_SUCCESS) return VK_NULL_HANDLE;
    return out;
}

VkDescriptorSetLayout samplerSetLayout(VkDevice dev, uint32_t samplers) {
    SceneVkDescriptorLayoutBuilder builder;
    for (uint32_t i = 0; i < samplers; ++i)
        builder.addBinding(i, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT);
    return builder.build(dev);
}

VkPipeline pipeline(VkDevice dev, VkPipelineLayout layout, VkShaderModule vs, VkShaderModule fs, VkFormat format) {
    if (!vs || !fs || !layout) return VK_NULL_HANDLE;
    SceneVkPipelineBuilder b;
    b.setShaderStages(vs, fs)
     .setInputTopology(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST)
     .setPolygonMode(VK_POLYGON_MODE_FILL)
     .setCullMode(VK_CULL_MODE_NONE)
     .setTarget(TargetFormat::colorOnly(format))
     .disableBlending(1)
     .disableDepthTest();
    return b.build(dev, layout);
}

void draw(SceneVkDevice& device, VkCommandBuffer cmd, const SceneVkImage& target, VkPipeline pipeline,
          VkPipelineLayout layout, VkDescriptorSet set, const void* push, uint32_t pushBytes) {
    VkRenderingAttachmentInfo att{};
    att.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
    att.imageView = target.view;
    att.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    att.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    VkRenderingInfo info{};
    info.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
    info.renderArea = {{0, 0}, {target.width, target.height}};
    info.layerCount = 1;
    info.colorAttachmentCount = 1;
    info.pColorAttachments = &att;
    device.cmdBeginRendering(cmd, &info);
    const VkViewport vp{0.0f, 0.0f, static_cast<float>(target.width), static_cast<float>(target.height), 0.0f, 1.0f};
    const VkRect2D scissor{{0, 0}, {target.width, target.height}};
    vkCmdSetViewport(cmd, 0, 1, &vp);
    vkCmdSetScissor(cmd, 0, 1, &scissor);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 0, 1, &set, 0, nullptr);
    if (push && pushBytes) vkCmdPushConstants(cmd, layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, pushBytes, push);
    vkCmdDraw(cmd, 3, 1, 0, 0);
    device.cmdEndRendering(cmd);
}

}  // namespace bro::scene::vk::fullscreen
