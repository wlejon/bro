#include "scene/vulkan/pass_billboard.h"
#include "scene/vulkan/scene_vk_shader_compiler.h"
#include "util/log.h"

namespace bro::scene::vk {

bool PassBillboard::init(SceneVkDevice& device, SceneVkAllocator& allocator,
                         VkDescriptorSetLayout cameraLayout,
                         VkDescriptorSetLayout materialLayout,
                         VkDescriptorSet defaultMaterialSet) {
    (void)allocator;
    VkDevice dev = device.device();
    cameraLayout_ = cameraLayout;
    materialLayout_ = materialLayout;
    defaultMaterialSet_ = defaultMaterialSet;

    // Pipeline Layout
    VkDescriptorSetLayout setLayouts[2] = {cameraLayout_, materialLayout_};
    VkPushConstantRange pushRange{};
    pushRange.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    pushRange.offset = 0;
    pushRange.size = sizeof(BillboardPushConstants);

    VkPipelineLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layoutInfo.setLayoutCount = 2;
    layoutInfo.pSetLayouts = setLayouts;
    layoutInfo.pushConstantRangeCount = 1;
    layoutInfo.pPushConstantRanges = &pushRange;

    if (vkCreatePipelineLayout(dev, &layoutInfo, nullptr, &pipelineLayout_) != VK_SUCCESS) {
        LOG_ERROR("PassBillboard: Failed creating pipeline layout");
        return false;
    }

    // Pipeline
    VkShaderModule vertMod = SceneVkShaderCompiler::createBuiltinModule(dev, BuiltinSceneShader::BillboardVert);
    VkShaderModule fragMod = SceneVkShaderCompiler::createBuiltinModule(dev, BuiltinSceneShader::BillboardFrag);
    if (!vertMod || !fragMod) {
        LOG_ERROR("PassBillboard: Failed creating shader modules");
        return false;
    }

    SceneVkPipelineBuilder b;
    b.addShaderStage(VK_SHADER_STAGE_VERTEX_BIT, vertMod)
     .addShaderStage(VK_SHADER_STAGE_FRAGMENT_BIT, fragMod)
     .setInputTopology(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST)
     .setPolygonMode(VK_POLYGON_MODE_FILL)
     .setCullMode(VK_CULL_MODE_NONE)
     .setMultisamplingNone();

    VkPipelineColorBlendAttachmentState blendAttachment{};
    blendAttachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                     VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    blendAttachment.blendEnable = VK_TRUE;
    blendAttachment.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
    blendAttachment.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    blendAttachment.colorBlendOp = VK_BLEND_OP_ADD;
    blendAttachment.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    blendAttachment.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    blendAttachment.alphaBlendOp = VK_BLEND_OP_ADD;
    b.setColorBlendAttachment(0, blendAttachment);

    b.enableDepthTest(false, VK_COMPARE_OP_GREATER_OR_EQUAL) // Reversed-Z
     .setDynamicRendering({VK_FORMAT_R16G16B16A16_SFLOAT}, VK_FORMAT_D32_SFLOAT);

    pipeline_ = b.build(dev, pipelineLayout_);

    SceneVkShaderCompiler::destroyModule(dev, vertMod);
    SceneVkShaderCompiler::destroyModule(dev, fragMod);

    return pipeline_ != VK_NULL_HANDLE;
}

void PassBillboard::cleanup(SceneVkDevice& device, SceneVkAllocator& allocator) {
    (void)allocator;
    VkDevice dev = device.device();
    if (pipeline_ != VK_NULL_HANDLE) {
        vkDestroyPipeline(dev, pipeline_, nullptr);
        pipeline_ = VK_NULL_HANDLE;
    }
    if (pipelineLayout_ != VK_NULL_HANDLE) {
        vkDestroyPipelineLayout(dev, pipelineLayout_, nullptr);
        pipelineLayout_ = VK_NULL_HANDLE;
    }
}

void PassBillboard::begin(VkCommandBuffer cmd, VkDescriptorSet cameraSet) {
    activeCameraSet_ = cameraSet;
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout_,
                            0, 1, &activeCameraSet_, 0, nullptr);
}

void PassBillboard::draw(VkCommandBuffer cmd, const BillboardPushConstants& push, VkDescriptorSet materialSet) {
    VkDescriptorSet mat = materialSet ? materialSet : defaultMaterialSet_;
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout_,
                            1, 1, &mat, 0, nullptr);
    vkCmdPushConstants(cmd, pipelineLayout_,
                       VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                       0, sizeof(push), &push);
    vkCmdDraw(cmd, 6, 1, 0, 0);
}

} // namespace bro::scene::vk
