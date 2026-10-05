#pragma once

#include "render/vulkan_context.h"
#include "webgl/vulkan/webgl_vk_types.h"

#include <vulkan/vulkan.h>
#include <vector>
#include <unordered_map>
#include <cstdint>

namespace bro::webgl::vk {

/// Key identifying the complete graphics pipeline state for WebGL draw calls.
struct PipelineKey {
    VkShaderModule vertShader = VK_NULL_HANDLE;
    VkShaderModule fragShader = VK_NULL_HANDLE;
    VkPrimitiveTopology topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkBool32 primitiveRestartEnable = VK_FALSE;
    VkBool32 depthBiasEnable = VK_FALSE;  // POLYGON_OFFSET_FILL; the factors are dynamic

    VkCullModeFlags cullMode = VK_CULL_MODE_BACK_BIT;
    VkFrontFace frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    VkBool32 cullFaceEnable = VK_FALSE;

    VkBool32 depthTestEnable = VK_FALSE;
    VkBool32 depthWriteEnable = VK_TRUE;
    VkCompareOp depthCompareOp = VK_COMPARE_OP_LESS;

    VkBool32 stencilTestEnable = VK_FALSE;
    VkStencilOpState stencilFront{};
    VkStencilOpState stencilBack{};

    VkBool32 blendEnable = VK_FALSE;
    VkBlendFactor srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
    VkBlendFactor dstColorBlendFactor = VK_BLEND_FACTOR_ZERO;
    VkBlendOp colorBlendOp = VK_BLEND_OP_ADD;
    VkBlendFactor srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    VkBlendFactor dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
    VkBlendOp alphaBlendOp = VK_BLEND_OP_ADD;

    VkColorComponentFlags colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                          VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    // Attachments that store an RGB format as RGBA: their alpha (one) is
    // never written.
    uint8_t alphaOneMask = 0;
    // Attachments the fragment shader does not write (an internal clear's).
    uint8_t unwrittenMask = 0;

    // Multisampling (only with more than one sample, as GL has it).
    VkBool32 alphaToCoverageEnable = VK_FALSE;
    uint32_t sampleMask = ~0u;

    // The pass's attachments: format i is fragment output i's (UNDEFINED for
    // a NONE draw buffer); depth and stencil are UNDEFINED when absent.
    uint32_t colorAttachmentCount = 1;
    VkFormat colorAttachmentFormats[8]{VK_FORMAT_R8G8B8A8_UNORM};
    VkFormat depthAttachmentFormat = VK_FORMAT_UNDEFINED;
    VkFormat stencilAttachmentFormat = VK_FORMAT_UNDEFINED;
    VkSampleCountFlagBits samples = VK_SAMPLE_COUNT_1_BIT;

    uint32_t attributeCount = 0;
    VkVertexInputAttributeDescription attributes[16]{};

    uint32_t bindingCount = 0;
    VkVertexInputBindingDescription bindings[16]{};
    // Per binding: the instance divisor when other than 0 or 1
    // (VK_EXT_vertex_attribute_divisor).
    uint32_t divisors[16]{};

    bool operator==(const PipelineKey& o) const;
};

struct PipelineKeyHasher {
    size_t operator()(const PipelineKey& k) const;
};

/// Cache of Vulkan dynamic rendering graphics pipelines keyed by WebGL render state.
class WebGLVkPipelineCache {
public:
    explicit WebGLVkPipelineCache(render::VulkanContext& context);
    ~WebGLVkPipelineCache();

    WebGLVkPipelineCache(const WebGLVkPipelineCache&) = delete;
    WebGLVkPipelineCache& operator=(const WebGLVkPipelineCache&) = delete;

    /// Retrieve or build a VkPipeline matching key and pipeline layout. Every
    /// pipeline takes viewport, scissor, depth bias and blend constants as
    /// dynamic state, and the line width too where the device has wide lines;
    /// the draw sets them.
    VkPipeline getOrCreatePipeline(const PipelineKey& key, VkPipelineLayout layout);

    /// Destroy all cached pipelines (the caller has waited for their use).
    void clear();

    /// Drop the pipelines built from `vert` or `frag` before those modules are
    /// destroyed — a later module can reuse the handle — destroying them once
    /// the GPU is done with them.
    void evictShaders(VkShaderModule vert, VkShaderModule frag);

private:
    VkPipeline createPipeline(const PipelineKey& key, VkPipelineLayout layout);

    render::VulkanContext& context_;
    std::unordered_map<PipelineKey, VkPipeline, PipelineKeyHasher> pipelines_;
};

} // namespace bro::webgl::vk
