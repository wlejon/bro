#include "webgl/vulkan/webgl_vk_pipeline.h"
#include "util/log.h"

#include <cstring>
#include <functional>

namespace bro::webgl::vk {

namespace {

inline void hashCombine(size_t& seed, size_t value) {
    value ^= value >> 30;
    value *= 0xbf58476d1ce4e5b9ULL;
    value ^= value >> 27;
    value *= 0x94d049bb133111ebULL;
    value ^= value >> 31;
    seed ^= value + 0x9e3779b97f4a7c15ULL + (seed << 6) + (seed >> 2);
}

} // namespace

bool PipelineKey::operator==(const PipelineKey& o) const {
    if (vertShader != o.vertShader || fragShader != o.fragShader || topology != o.topology) return false;
    if (primitiveRestartEnable != o.primitiveRestartEnable ||
        depthBiasEnable != o.depthBiasEnable || alphaOneMask != o.alphaOneMask || unwrittenMask != o.unwrittenMask ||
        alphaToCoverageEnable != o.alphaToCoverageEnable || sampleMask != o.sampleMask)
        return false;
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
    if (depthAttachmentFormat != o.depthAttachmentFormat || stencilAttachmentFormat != o.stencilAttachmentFormat ||
        samples != o.samples)
        return false;
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
            bindings[i].inputRate != o.bindings[i].inputRate || divisors[i] != o.divisors[i]) {
            return false;
        }
    }

    return true;
}

size_t PipelineKeyHasher::operator()(const PipelineKey& k) const {
    size_t seed = 0;
    hashCombine(seed, reinterpret_cast<uintptr_t>(k.vertShader));
    hashCombine(seed, reinterpret_cast<uintptr_t>(k.fragShader));
    const uint64_t packedState =
        static_cast<uint64_t>(k.topology) |
        (static_cast<uint64_t>(k.primitiveRestartEnable & 1u) << 4) |
        (static_cast<uint64_t>(k.depthBiasEnable & 1u) << 5) |
        (static_cast<uint64_t>(k.alphaToCoverageEnable & 1u) << 6) |
        (static_cast<uint64_t>(k.cullFaceEnable & 1u) << 7) |
        (static_cast<uint64_t>(k.cullMode & 0xFu) << 8) |
        (static_cast<uint64_t>(k.frontFace & 0x3u) << 12) |
        (static_cast<uint64_t>(k.depthTestEnable & 1u) << 14) |
        (static_cast<uint64_t>(k.depthWriteEnable & 1u) << 15) |
        (static_cast<uint64_t>(k.depthCompareOp & 0xFu) << 16) |
        (static_cast<uint64_t>(k.stencilTestEnable & 1u) << 20) |
        (static_cast<uint64_t>(k.blendEnable & 1u) << 21) |
        (static_cast<uint64_t>(k.colorWriteMask & 0xFu) << 22) |
        (static_cast<uint64_t>(k.colorAttachmentCount & 0xFu) << 26) |
        (static_cast<uint64_t>(k.samples & 0x7Fu) << 30) |
        (static_cast<uint64_t>(k.alphaOneMask) << 40) |
        (static_cast<uint64_t>(k.unwrittenMask) << 48) |
        (static_cast<uint64_t>(k.attributeCount & 0x1Fu) << 56) |
        (static_cast<uint64_t>(k.bindingCount & 0x1Fu) << 59);
    hashCombine(seed, packedState);
    hashCombine(seed, k.sampleMask);
    if (k.blendEnable) {
        const uint64_t blendPacked =
            static_cast<uint64_t>(k.srcColorBlendFactor) |
            (static_cast<uint64_t>(k.dstColorBlendFactor) << 8) |
            (static_cast<uint64_t>(k.colorBlendOp) << 16) |
            (static_cast<uint64_t>(k.srcAlphaBlendFactor) << 24) |
            (static_cast<uint64_t>(k.dstAlphaBlendFactor) << 32) |
            (static_cast<uint64_t>(k.alphaBlendOp) << 40);
        hashCombine(seed, blendPacked);
    }
    if (k.stencilTestEnable) {
        hashCombine(seed, (static_cast<uint64_t>(k.stencilFront.compareOp) << 32) | k.stencilFront.reference);
        hashCombine(seed, (static_cast<uint64_t>(k.stencilFront.compareMask) << 32) | k.stencilFront.writeMask);
    }
    hashCombine(seed, (static_cast<uint64_t>(k.depthAttachmentFormat) << 32) |
                          static_cast<uint32_t>(k.stencilAttachmentFormat));
    for (uint32_t i = 0; i < k.colorAttachmentCount; ++i) {
        hashCombine(seed, static_cast<uint32_t>(k.colorAttachmentFormats[i]));
    }
    for (uint32_t i = 0; i < k.attributeCount; ++i) {
        hashCombine(seed, (static_cast<uint64_t>(k.attributes[i].location) << 32) |
                              static_cast<uint32_t>(k.attributes[i].format));
    }
    for (uint32_t i = 0; i < k.bindingCount; ++i) {
        hashCombine(seed, (static_cast<uint64_t>(k.bindings[i].stride) << 32) |
                              (static_cast<uint32_t>(k.bindings[i].inputRate) << 16) | k.divisors[i]);
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
    lastPipeline_ = VK_NULL_HANDLE;
    lastLayout_ = VK_NULL_HANDLE;
}

void WebGLVkPipelineCache::evictShaders(VkShaderModule vert, VkShaderModule frag) {
    VkDevice dev = context_.device();
    if (lastKey_.vertShader == vert || lastKey_.fragShader == frag) {
        lastPipeline_ = VK_NULL_HANDLE;
        lastLayout_ = VK_NULL_HANDLE;
    }
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
    if (lastPipeline_ != VK_NULL_HANDLE && lastLayout_ == layout && lastKey_ == key) {
        return lastPipeline_;
    }
    auto it = pipelines_.find(key);
    if (it != pipelines_.end()) {
        lastKey_ = key;
        lastLayout_ = layout;
        lastPipeline_ = it->second;
        return it->second;
    }

    VkPipeline pipeline = createPipeline(key, layout);
    if (pipeline != VK_NULL_HANDLE) {
        pipelines_[key] = pipeline;
        lastKey_ = key;
        lastLayout_ = layout;
        lastPipeline_ = pipeline;
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
    inputAssembly.primitiveRestartEnable = key.primitiveRestartEnable;

    VkVertexInputBindingDivisorDescriptionEXT divisors[16]{};
    VkPipelineVertexInputDivisorStateCreateInfoEXT divisorInfo{};
    divisorInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_DIVISOR_STATE_CREATE_INFO_EXT;
    for (uint32_t i = 0; i < key.bindingCount; ++i)
        if (key.divisors[i] > 1) divisors[divisorInfo.vertexBindingDivisorCount++] = {i, key.divisors[i]};
    if (divisorInfo.vertexBindingDivisorCount > 0) {
        divisorInfo.pVertexBindingDivisors = divisors;
        vertexInputInfo.pNext = &divisorInfo;
    }

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
    rasterizer.depthBiasEnable = key.depthBiasEnable;

    VkPipelineMultisampleStateCreateInfo multisampling{};
    multisampling.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    multisampling.sampleShadingEnable = VK_FALSE;
    multisampling.rasterizationSamples = key.samples;
    multisampling.alphaToCoverageEnable = key.alphaToCoverageEnable;
    const VkSampleMask sampleMask = key.sampleMask;
    multisampling.pSampleMask = &sampleMask;

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

    const uint32_t attCount = std::min(8u, key.colorAttachmentCount);
    VkPipelineColorBlendAttachmentState colorBlendAttachments[8]{};
    for (uint32_t i = 0; i < attCount; ++i) {
        colorBlendAttachments[i] = colorBlendAttachment;
        if (key.alphaOneMask & (1u << i)) colorBlendAttachments[i].colorWriteMask &= ~VK_COLOR_COMPONENT_A_BIT;
        if (key.unwrittenMask & (1u << i)) colorBlendAttachments[i].colorWriteMask = 0;
        // GL does not blend into integer (or otherwise unblendable) buffers.
        if (key.blendEnable && key.colorAttachmentFormats[i] != VK_FORMAT_UNDEFINED) {
            VkFormatProperties props;
            vkGetPhysicalDeviceFormatProperties(context_.physicalDevice(), key.colorAttachmentFormats[i], &props);
            if (!(props.optimalTilingFeatures & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT))
                colorBlendAttachments[i].blendEnable = VK_FALSE;
        }
    }

    VkPipelineColorBlendStateCreateInfo colorBlending{};
    colorBlending.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    colorBlending.logicOpEnable = VK_FALSE;
    colorBlending.attachmentCount = attCount;
    colorBlending.pAttachments = colorBlendAttachments;

    VkDynamicState dynamicStates[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR,
                                      VK_DYNAMIC_STATE_DEPTH_BIAS, VK_DYNAMIC_STATE_BLEND_CONSTANTS,
                                      VK_DYNAMIC_STATE_LINE_WIDTH};
    VkPipelineDynamicStateCreateInfo dynamicState{};
    dynamicState.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynamicState.dynamicStateCount = context_.wideLines() ? 5 : 4;
    dynamicState.pDynamicStates = dynamicStates;

    // Dynamic Rendering configuration
    VkPipelineRenderingCreateInfoKHR renderingCreateInfo{};
    renderingCreateInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO_KHR;
    renderingCreateInfo.colorAttachmentCount = attCount;
    renderingCreateInfo.pColorAttachmentFormats = key.colorAttachmentFormats;
    renderingCreateInfo.depthAttachmentFormat = key.depthAttachmentFormat;
    renderingCreateInfo.stencilAttachmentFormat = key.stencilAttachmentFormat;

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
