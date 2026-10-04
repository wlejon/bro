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

    uint32_t colorAttachmentCount = 1;
    VkFormat colorAttachmentFormats[8]{VK_FORMAT_R8G8B8A8_UNORM};
    VkFormat colorAttachmentFormat = VK_FORMAT_R8G8B8A8_UNORM;
    VkFormat depthAttachmentFormat = VK_FORMAT_UNDEFINED;

    uint32_t attributeCount = 0;
    VkVertexInputAttributeDescription attributes[16]{};

    uint32_t bindingCount = 0;
    VkVertexInputBindingDescription bindings[16]{};

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

    /// Retrieve or build a VkPipeline matching key and pipeline layout.
    VkPipeline getOrCreatePipeline(const PipelineKey& key, VkPipelineLayout layout);

    /// Destroy all cached pipelines.
    void clear();

private:
    VkPipeline createPipeline(const PipelineKey& key, VkPipelineLayout layout);

    render::VulkanContext& context_;
    std::unordered_map<PipelineKey, VkPipeline, PipelineKeyHasher> pipelines_;
};

} // namespace bro::webgl::vk
