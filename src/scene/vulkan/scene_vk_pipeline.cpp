#include "scene/vulkan/scene_vk_pipeline.h"
#include "render/vulkan_pipeline_cache.h"
#include "util/log.h"

#include <cassert>
#include <cstring>

namespace bro::scene::vk {

VkShaderModule SceneVkShaderModule::create(VkDevice device, const uint32_t* code, size_t sizeBytes) {
    if (!device || !code || sizeBytes == 0) return VK_NULL_HANDLE;

    VkShaderModuleCreateInfo createInfo{};
    createInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    createInfo.codeSize = sizeBytes;
    createInfo.pCode = code;

    VkShaderModule module = VK_NULL_HANDLE;
    if (vkCreateShaderModule(device, &createInfo, nullptr, &module) != VK_SUCCESS) {
        LOG_ERROR("SceneVkShaderModule: Failed to create shader module (%zu bytes)", sizeBytes);
        return VK_NULL_HANDLE;
    }
    return module;
}

VkShaderModule SceneVkShaderModule::create(VkDevice device, const std::vector<uint32_t>& spirv) {
    return create(device, spirv.data(), spirv.size() * sizeof(uint32_t));
}

void SceneVkShaderModule::destroy(VkDevice device, VkShaderModule module) {
    if (device != VK_NULL_HANDLE && module != VK_NULL_HANDLE) {
        vkDestroyShaderModule(device, module, nullptr);
    }
}

SceneVkPipelineBuilder::SceneVkPipelineBuilder() {
    reset();
}

SceneVkPipelineBuilder& SceneVkPipelineBuilder::reset() {
    shaderStages_.clear();
    vertexBindings_.clear();
    vertexAttributes_.clear();

    inputAssembly_ = {};
    inputAssembly_.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    inputAssembly_.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    inputAssembly_.primitiveRestartEnable = VK_FALSE;

    rasterizer_ = {};
    rasterizer_.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rasterizer_.depthClampEnable = VK_FALSE;
    rasterizer_.rasterizerDiscardEnable = VK_FALSE;
    rasterizer_.polygonMode = VK_POLYGON_MODE_FILL;
    rasterizer_.lineWidth = 1.0f;
    rasterizer_.cullMode = VK_CULL_MODE_BACK_BIT;
    rasterizer_.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rasterizer_.depthBiasEnable = VK_FALSE;

    multisampling_ = {};
    multisampling_.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    multisampling_.sampleShadingEnable = VK_FALSE;
    multisampling_.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    depthStencil_ = {};
    depthStencil_.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    depthStencil_.depthTestEnable = VK_TRUE;
    depthStencil_.depthWriteEnable = VK_TRUE;
    depthStencil_.depthCompareOp = depth::compareCloser();
    depthStencil_.depthBoundsTestEnable = VK_FALSE;
    depthStencil_.stencilTestEnable = VK_FALSE;

    colorBlendAttachments_.clear();
    disableBlending(1);

    dynamicStates_ = {
        VK_DYNAMIC_STATE_VIEWPORT,
        VK_DYNAMIC_STATE_SCISSOR
    };

    colorAttachmentFormats_.clear();
    depthAttachmentFormat_ = VK_FORMAT_UNDEFINED;
    stencilAttachmentFormat_ = VK_FORMAT_UNDEFINED;

    return *this;
}

SceneVkPipelineBuilder& SceneVkPipelineBuilder::addShaderStage(VkShaderStageFlagBits stage,
                                                              VkShaderModule module,
                                                              const char* entryPoint) {
    VkPipelineShaderStageCreateInfo stageInfo{};
    stageInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stageInfo.stage = stage;
    stageInfo.module = module;
    stageInfo.pName = entryPoint;
    shaderStages_.push_back(stageInfo);
    return *this;
}

SceneVkPipelineBuilder& SceneVkPipelineBuilder::setVertexInput(
    const std::vector<VkVertexInputBindingDescription>& bindings,
    const std::vector<VkVertexInputAttributeDescription>& attributes) {
    vertexBindings_ = bindings;
    vertexAttributes_ = attributes;
    return *this;
}

SceneVkPipelineBuilder& SceneVkPipelineBuilder::setInputTopology(VkPrimitiveTopology topology, bool primitiveRestart) {
    inputAssembly_.topology = topology;
    inputAssembly_.primitiveRestartEnable = primitiveRestart ? VK_TRUE : VK_FALSE;
    return *this;
}

SceneVkPipelineBuilder& SceneVkPipelineBuilder::setPolygonMode(VkPolygonMode mode, float lineWidth) {
    rasterizer_.polygonMode = mode;
    rasterizer_.lineWidth = lineWidth;
    return *this;
}

SceneVkPipelineBuilder& SceneVkPipelineBuilder::setCullMode(VkCullModeFlags cullMode, VkFrontFace frontFace) {
    rasterizer_.cullMode = cullMode;
    rasterizer_.frontFace = frontFace;
    return *this;
}

SceneVkPipelineBuilder& SceneVkPipelineBuilder::setDepthBias(bool enable) {
    rasterizer_.depthBiasEnable = enable ? VK_TRUE : VK_FALSE;
    return *this;
}

SceneVkPipelineBuilder& SceneVkPipelineBuilder::setTarget(const TargetFormat& target) {
    colorAttachmentFormats_.assign(target.color, target.color + target.colorCount);
    depthAttachmentFormat_ = target.depth;
    stencilAttachmentFormat_ = VK_FORMAT_UNDEFINED;
    multisampling_.sampleShadingEnable = VK_FALSE;
    multisampling_.rasterizationSamples = target.samples;
    return *this;
}

SceneVkPipelineBuilder& SceneVkPipelineBuilder::disableBlending(uint32_t colorAttachmentCount) {
    colorBlendAttachments_.clear();
    for (uint32_t i = 0; i < colorAttachmentCount; ++i) {
        VkPipelineColorBlendAttachmentState blendAttachment{};
        blendAttachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                         VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
        blendAttachment.blendEnable = VK_FALSE;
        colorBlendAttachments_.push_back(blendAttachment);
    }
    return *this;
}

SceneVkPipelineBuilder& SceneVkPipelineBuilder::enableAlphaBlending(uint32_t colorAttachmentCount) {
    colorBlendAttachments_.clear();
    for (uint32_t i = 0; i < colorAttachmentCount; ++i) {
        VkPipelineColorBlendAttachmentState blendAttachment{};
        blendAttachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                         VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
        blendAttachment.blendEnable = VK_TRUE;
        blendAttachment.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
        blendAttachment.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        blendAttachment.colorBlendOp = VK_BLEND_OP_ADD;
        blendAttachment.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        blendAttachment.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        blendAttachment.alphaBlendOp = VK_BLEND_OP_ADD;
        colorBlendAttachments_.push_back(blendAttachment);
    }
    return *this;
}

SceneVkPipelineBuilder& SceneVkPipelineBuilder::setColorBlendAttachment(
    uint32_t index, const VkPipelineColorBlendAttachmentState& blendState) {
    if (index >= colorBlendAttachments_.size()) {
        colorBlendAttachments_.resize(index + 1);
    }
    colorBlendAttachments_[index] = blendState;
    return *this;
}

SceneVkPipelineBuilder& SceneVkPipelineBuilder::enableDepthTest(bool depthWrite, VkCompareOp compareOp) {
    depthStencil_.depthTestEnable = VK_TRUE;
    depthStencil_.depthWriteEnable = depthWrite ? VK_TRUE : VK_FALSE;
    depthStencil_.depthCompareOp = compareOp;
    return *this;
}

SceneVkPipelineBuilder& SceneVkPipelineBuilder::disableDepthTest() {
    depthStencil_.depthTestEnable = VK_FALSE;
    depthStencil_.depthWriteEnable = VK_FALSE;
    return *this;
}

SceneVkPipelineBuilder& SceneVkPipelineBuilder::setDynamicStates(const std::vector<VkDynamicState>& states) {
    dynamicStates_ = states;
    return *this;
}

VkPipeline SceneVkPipelineBuilder::build(VkDevice device, VkPipelineLayout layout) {
    VkPipelineVertexInputStateCreateInfo vertexInputInfo{};
    vertexInputInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vertexInputInfo.vertexBindingDescriptionCount = static_cast<uint32_t>(vertexBindings_.size());
    vertexInputInfo.pVertexBindingDescriptions = vertexBindings_.data();
    vertexInputInfo.vertexAttributeDescriptionCount = static_cast<uint32_t>(vertexAttributes_.size());
    vertexInputInfo.pVertexAttributeDescriptions = vertexAttributes_.data();

    VkPipelineViewportStateCreateInfo viewportState{};
    viewportState.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    viewportState.viewportCount = 1;
    viewportState.pViewports = nullptr; // Handled dynamically
    viewportState.scissorCount = 1;
    viewportState.pScissors = nullptr;   // Handled dynamically

    // One blend state per colour attachment. Without independentBlend (not
    // enabled on the device) they must all be identical, so an attachment the
    // pass did not set blends like the first — the shader writes every
    // output of a multi-attachment target.
    std::vector<VkPipelineColorBlendAttachmentState> blends = colorBlendAttachments_;
    if (!blends.empty()) blends.resize(colorAttachmentFormats_.size(), blends.front());

    VkPipelineColorBlendStateCreateInfo colorBlending{};
    colorBlending.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    colorBlending.logicOpEnable = VK_FALSE;
    colorBlending.logicOp = VK_LOGIC_OP_COPY;
    colorBlending.attachmentCount = static_cast<uint32_t>(blends.size());
    colorBlending.pAttachments = blends.data();

    // The depth policy, as specialization constant 0 on every stage.
    const VkBool32 reversed = reversedZ() ? VK_TRUE : VK_FALSE;
    const VkSpecializationMapEntry policyEntry{depth::kReversedZConstantId, 0, sizeof(VkBool32)};
    VkSpecializationInfo policy{};
    policy.mapEntryCount = 1;
    policy.pMapEntries = &policyEntry;
    policy.dataSize = sizeof(reversed);
    policy.pData = &reversed;
    std::vector<VkPipelineShaderStageCreateInfo> stages = shaderStages_;
    for (auto& st : stages) st.pSpecializationInfo = &policy;

    VkPipelineDynamicStateCreateInfo dynamicState{};
    dynamicState.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynamicState.dynamicStateCount = static_cast<uint32_t>(dynamicStates_.size());
    dynamicState.pDynamicStates = dynamicStates_.data();

    VkGraphicsPipelineCreateInfo pipelineInfo{};
    pipelineInfo.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    pipelineInfo.stageCount = static_cast<uint32_t>(stages.size());
    pipelineInfo.pStages = stages.data();
    pipelineInfo.pVertexInputState = &vertexInputInfo;
    pipelineInfo.pInputAssemblyState = &inputAssembly_;
    pipelineInfo.pViewportState = &viewportState;
    pipelineInfo.pRasterizationState = &rasterizer_;
    pipelineInfo.pMultisampleState = &multisampling_;
    pipelineInfo.pDepthStencilState = &depthStencil_;
    pipelineInfo.pColorBlendState = &colorBlending;
    pipelineInfo.pDynamicState = &dynamicState;
    pipelineInfo.layout = layout;
    pipelineInfo.renderPass = VK_NULL_HANDLE; // Dynamic rendering does not use VkRenderPass
    pipelineInfo.subpass = 0;

    // Vulkan 1.3 Dynamic Rendering info chained in pNext
    VkPipelineRenderingCreateInfoKHR renderingInfo{};
    renderingInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO_KHR;
    renderingInfo.colorAttachmentCount = static_cast<uint32_t>(colorAttachmentFormats_.size());
    renderingInfo.pColorAttachmentFormats = colorAttachmentFormats_.empty() ? nullptr : colorAttachmentFormats_.data();
    renderingInfo.depthAttachmentFormat = depthAttachmentFormat_;
    renderingInfo.stencilAttachmentFormat = stencilAttachmentFormat_;

    pipelineInfo.pNext = &renderingInfo;

    VkPipeline pipeline = VK_NULL_HANDLE;
    VkResult res = vkCreateGraphicsPipelines(device, render::VulkanPipelineCache::forDevice(device), 1,
                                             &pipelineInfo, nullptr, &pipeline);
    if (res != VK_SUCCESS) {
        LOG_ERROR("SceneVkPipelineBuilder: Failed to create graphics pipeline: %d", res);
        return VK_NULL_HANDLE;
    }

    return pipeline;
}

} // namespace bro::scene::vk
