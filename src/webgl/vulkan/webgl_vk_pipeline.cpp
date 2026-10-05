#include "webgl/vulkan/webgl_vk_pipeline.h"
#include "util/log.h"

#include <cstring>
#include <functional>

namespace bro::webgl::vk {

namespace {

inline void hashCombine(size_t& seed, size_t value) {
    seed ^= value + 0x9e3779b9 + (seed << 6) + (seed >> 2);
}

} // namespace

bool PipelineKey::operator==(const PipelineKey& o) const {
    if (vertShader != o.vertShader || fragShader != o.fragShader || topology != o.topology) return false;
    if (cullMode != o.cullMode || frontFace != o.frontFace || cullFaceEnable != o.cullFaceEnable) return false;
    if (depthTestEnable != o.depthTestEnable || depthWriteEnable != o.depthWriteEnable || depthCompareOp != o.depthCompareOp) return false;
    if (stencilTestEnable != o.stencilTestEnable) return false;
    if (stencilTestEnable) {
        if (std::memcmp(&stencilFront, &o.stencilFront, sizeof(VkStencilOpState)) != 0 ||
            std::memcmp(&stencilBack, &o.stencilBack, sizeof(VkStencilOpState)) != 0) return false;
    }
    if (blendEnable != o.blendEnable) return false;
    if (srcColorBlendFactor != o.srcColorBlendFactor || dstColorBlendFactor != o.dstColorBlendFactor || colorBlendOp != o.colorBlendOp) return false;
    if (srcAlphaBlendFactor != o.srcAlphaBlendFactor || dstAlphaBlendFactor != o.dstAlphaBlendFactor || alphaBlendOp != o.alphaBlendOp) return false;
    if (colorWriteMask != o.colorWriteMask) return false;
    if (colorAttachmentCount != o.colorAttachmentCount) return false;
    for (uint32_t i = 0; i < colorAttachmentCount; ++i) {
        if (colorAttachmentFormats[i] != o.colorAttachmentFormats[i]) return false;
    }
    if (depthAttachmentFormat != o.depthAttachmentFormat) return false;
    if (attributeCount != o.attributeCount || bindingCount != o.bindingCount) return false;

    for (uint32_t i = 0; i < attributeCount; ++i) {
        if (attributes[i].location != o.attributes[i].location ||
            attributes[i].binding != o.attributes[i].binding ||
            attributes[i].format != o.attributes[i].format ||
            attributes[i].offset != o.attributes[i].offset) {
            return false;
        }
    }

    for (uint32_t i = 0; i < bindingCount; ++i) {
        if (bindings[i].binding != o.bindings[i].binding ||
            bindings[i].stride != o.bindings[i].stride ||
            bindings[i].inputRate != o.bindings[i].inputRate) {
            return false;
        }
    }

    return true;
}

size_t PipelineKeyHasher::operator()(const PipelineKey& k) const {
    size_t seed = 0;
    hashCombine(seed, std::hash<void*>()((void*)k.vertShader));
    hashCombine(seed, std::hash<void*>()((void*)k.fragShader));
    hashCombine(seed, std::hash<uint32_t>()(static_cast<uint32_t>(k.topology)));
    hashCombine(seed, std::hash<uint32_t>()(static_cast<uint32_t>(k.cullMode)));
    hashCombine(seed, std::hash<uint32_t>()(static_cast<uint32_t>(k.frontFace)));
    hashCombine(seed, std::hash<uint32_t>()(k.cullFaceEnable));
    hashCombine(seed, std::hash<uint32_t>()(k.depthTestEnable));
    hashCombine(seed, std::hash<uint32_t>()(k.depthWriteEnable));
    hashCombine(seed, std::hash<uint32_t>()(static_cast<uint32_t>(k.depthCompareOp)));
    hashCombine(seed, std::hash<uint32_t>()(k.stencilTestEnable));
    if (k.stencilTestEnable) {
        hashCombine(seed, std::hash<uint32_t>()(static_cast<uint32_t>(k.stencilFront.compareOp)));
        hashCombine(seed, std::hash<uint32_t>()(k.stencilFront.reference));
        hashCombine(seed, std::hash<uint32_t>()(k.stencilFront.compareMask));
        hashCombine(seed, std::hash<uint32_t>()(k.stencilFront.writeMask));
    }
    hashCombine(seed, std::hash<uint32_t>()(k.blendEnable));
    hashCombine(seed, std::hash<uint32_t>()(static_cast<uint32_t>(k.srcColorBlendFactor)));
    hashCombine(seed, std::hash<uint32_t>()(static_cast<uint32_t>(k.dstColorBlendFactor)));
    hashCombine(seed, std::hash<uint32_t>()(static_cast<uint32_t>(k.colorWriteMask)));
    hashCombine(seed, std::hash<uint32_t>()(k.colorAttachmentCount));
    for (uint32_t i = 0; i < k.colorAttachmentCount; ++i) {
        hashCombine(seed, std::hash<uint32_t>()(static_cast<uint32_t>(k.colorAttachmentFormats[i])));
    }
    hashCombine(seed, std::hash<uint32_t>()(k.attributeCount));
    hashCombine(seed, std::hash<uint32_t>()(k.bindingCount));
    for (uint32_t i = 0; i < k.attributeCount; ++i) {
        hashCombine(seed, std::hash<uint32_t>()(k.attributes[i].location));
        hashCombine(seed, std::hash<uint32_t>()(static_cast<uint32_t>(k.attributes[i].format)));
    }
    return seed;
}

WebGLVkPipelineCache::WebGLVkPipelineCache(render::VulkanContext& context)
    : context_(context)
{
}

WebGLVkPipelineCache::~WebGLVkPipelineCache() {
    clear();
}

void WebGLVkPipelineCache::clear() {
    VkDevice dev = context_.device();
    if (dev != VK_NULL_HANDLE) {
        for (auto& pair : pipelines_) {
            if (pair.second != VK_NULL_HANDLE) {
                vkDestroyPipeline(dev, pair.second, nullptr);
            }
        }
    }
    pipelines_.clear();
}

void WebGLVkPipelineCache::evictShaders(VkShaderModule vert, VkShaderModule frag) {
    VkDevice dev = context_.device();
    for (auto it = pipelines_.begin(); it != pipelines_.end();) {
        if (it->first.vertShader == vert || it->first.fragShader == frag) {
            VkPipeline pipeline = it->second;
            context_.frames().defer([dev, pipeline] { vkDestroyPipeline(dev, pipeline, nullptr); });
            it = pipelines_.erase(it);
        } else {
            ++it;
        }
    }
}

VkPipeline WebGLVkPipelineCache::getOrCreatePipeline(const PipelineKey& key, VkPipelineLayout layout) {
    auto it = pipelines_.find(key);
    if (it != pipelines_.end()) {
        return it->second;
    }

    VkPipeline pipeline = createPipeline(key, layout);
    if (pipeline != VK_NULL_HANDLE) {
        pipelines_[key] = pipeline;
    }
    return pipeline;
}

VkPipeline WebGLVkPipelineCache::createPipeline(const PipelineKey& key, VkPipelineLayout layout) {
    VkDevice dev = context_.device();

    VkPipelineShaderStageCreateInfo shaderStages[2]{};
    shaderStages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    shaderStages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    shaderStages[0].module = key.vertShader;
    shaderStages[0].pName = "main";

    shaderStages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    shaderStages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    shaderStages[1].module = key.fragShader;
    shaderStages[1].pName = "main";

    VkPipelineVertexInputStateCreateInfo vertexInputInfo{};
    vertexInputInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vertexInputInfo.vertexBindingDescriptionCount = key.bindingCount;
    vertexInputInfo.pVertexBindingDescriptions = key.bindings;
    vertexInputInfo.vertexAttributeDescriptionCount = key.attributeCount;
    vertexInputInfo.pVertexAttributeDescriptions = key.attributes;

    VkPipelineInputAssemblyStateCreateInfo inputAssembly{};
    inputAssembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    inputAssembly.topology = key.topology;
    inputAssembly.primitiveRestartEnable = VK_FALSE;

    VkPipelineViewportStateCreateInfo viewportState{};
    viewportState.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    viewportState.viewportCount = 1;
    viewportState.scissorCount = 1;

    VkPipelineRasterizationStateCreateInfo rasterizer{};
    rasterizer.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rasterizer.depthClampEnable = VK_FALSE;
    rasterizer.rasterizerDiscardEnable = VK_FALSE;
    rasterizer.polygonMode = VK_POLYGON_MODE_FILL;
    rasterizer.lineWidth = 1.0f;
    rasterizer.cullMode = key.cullFaceEnable ? key.cullMode : VK_CULL_MODE_NONE;
    rasterizer.frontFace = key.frontFace;
    rasterizer.depthBiasEnable = VK_FALSE;

    VkPipelineMultisampleStateCreateInfo multisampling{};
    multisampling.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    multisampling.sampleShadingEnable = VK_FALSE;
    multisampling.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineDepthStencilStateCreateInfo depthStencil{};
    depthStencil.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    depthStencil.depthTestEnable = key.depthTestEnable;
    depthStencil.depthWriteEnable = key.depthWriteEnable;
    depthStencil.depthCompareOp = key.depthCompareOp;
    depthStencil.depthBoundsTestEnable = VK_FALSE;
    depthStencil.stencilTestEnable = key.stencilTestEnable;
    depthStencil.front = key.stencilFront;
    depthStencil.back = key.stencilBack;

    VkPipelineColorBlendAttachmentState colorBlendAttachment{};
    colorBlendAttachment.colorWriteMask = key.colorWriteMask;
    colorBlendAttachment.blendEnable = key.blendEnable;
    colorBlendAttachment.srcColorBlendFactor = key.srcColorBlendFactor;
    colorBlendAttachment.dstColorBlendFactor = key.dstColorBlendFactor;
    colorBlendAttachment.colorBlendOp = key.colorBlendOp;
    colorBlendAttachment.srcAlphaBlendFactor = key.srcAlphaBlendFactor;
    colorBlendAttachment.dstAlphaBlendFactor = key.dstAlphaBlendFactor;
    colorBlendAttachment.alphaBlendOp = key.alphaBlendOp;

    uint32_t attCount = std::max(1u, std::min(8u, key.colorAttachmentCount));
    VkPipelineColorBlendAttachmentState colorBlendAttachments[8]{};
    for (uint32_t i = 0; i < attCount; ++i) {
        colorBlendAttachments[i] = colorBlendAttachment;
    }

    VkPipelineColorBlendStateCreateInfo colorBlending{};
    colorBlending.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    colorBlending.logicOpEnable = VK_FALSE;
    colorBlending.attachmentCount = attCount;
    colorBlending.pAttachments = colorBlendAttachments;

    VkDynamicState dynamicStates[] = {
        VK_DYNAMIC_STATE_VIEWPORT,
        VK_DYNAMIC_STATE_SCISSOR
    };
    VkPipelineDynamicStateCreateInfo dynamicState{};
    dynamicState.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynamicState.dynamicStateCount = 2;
    dynamicState.pDynamicStates = dynamicStates;

    // Dynamic Rendering configuration
    VkPipelineRenderingCreateInfoKHR renderingCreateInfo{};
    renderingCreateInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO_KHR;
    renderingCreateInfo.colorAttachmentCount = attCount;
    renderingCreateInfo.pColorAttachmentFormats = key.colorAttachmentFormats;
    renderingCreateInfo.depthAttachmentFormat = key.depthAttachmentFormat;
    if (key.depthAttachmentFormat == VK_FORMAT_D32_SFLOAT_S8_UINT ||
        key.depthAttachmentFormat == VK_FORMAT_D24_UNORM_S8_UINT) {
        renderingCreateInfo.stencilAttachmentFormat = key.depthAttachmentFormat;
    }

    VkGraphicsPipelineCreateInfo pipelineInfo{};
    pipelineInfo.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    pipelineInfo.pNext = &renderingCreateInfo;
    pipelineInfo.stageCount = 2;
    pipelineInfo.pStages = shaderStages;
    pipelineInfo.pVertexInputState = &vertexInputInfo;
    pipelineInfo.pInputAssemblyState = &inputAssembly;
    pipelineInfo.pViewportState = &viewportState;
    pipelineInfo.pRasterizationState = &rasterizer;
    pipelineInfo.pMultisampleState = &multisampling;
    pipelineInfo.pDepthStencilState = &depthStencil;
    pipelineInfo.pColorBlendState = &colorBlending;
    pipelineInfo.pDynamicState = &dynamicState;
    pipelineInfo.layout = layout;
    pipelineInfo.renderPass = VK_NULL_HANDLE; // dynamic rendering!
    pipelineInfo.subpass = 0;

    VkPipeline pipeline = VK_NULL_HANDLE;
    if (vkCreateGraphicsPipelines(dev, context_.pipelineCache(), 1, &pipelineInfo, nullptr, &pipeline) != VK_SUCCESS) {
        LOG_ERROR("WebGLVkPipelineCache: Failed to create graphics pipeline");
        return VK_NULL_HANDLE;
    }

    return pipeline;
}

} // namespace bro::webgl::vk
