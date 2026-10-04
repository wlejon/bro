#pragma once

#include <vulkan/vulkan.h>
#include <cstdint>
#include <string>
#include <vector>

namespace bro::scene::vk {

/// Helper for Vulkan shader module creation from SPIR-V bytecode.
class SceneVkShaderModule {
public:
    static VkShaderModule create(VkDevice device, const uint32_t* code, size_t sizeBytes);
    static VkShaderModule create(VkDevice device, const std::vector<uint32_t>& spirv);
    static void destroy(VkDevice device, VkShaderModule module);
};

/// Fluent graphics pipeline builder supporting Vulkan 1.3 Dynamic Rendering.
class SceneVkPipelineBuilder {
public:
    SceneVkPipelineBuilder();
    ~SceneVkPipelineBuilder() = default;

    /// Reset all state to default values.
    SceneVkPipelineBuilder& reset();

    /// Attach a shader stage module.
    SceneVkPipelineBuilder& addShaderStage(VkShaderStageFlagBits stage,
                                          VkShaderModule module,
                                          const char* entryPoint = "main");

    /// Specify vertex input binding and attribute layouts.
    SceneVkPipelineBuilder& setVertexInput(const std::vector<VkVertexInputBindingDescription>& bindings,
                                          const std::vector<VkVertexInputAttributeDescription>& attributes);

    /// Set primitive topology (e.g. VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST).
    SceneVkPipelineBuilder& setInputTopology(VkPrimitiveTopology topology, bool primitiveRestart = false);

    /// Set polygon fill mode (e.g. VK_POLYGON_MODE_FILL, VK_POLYGON_MODE_LINE).
    SceneVkPipelineBuilder& setPolygonMode(VkPolygonMode mode, float lineWidth = 1.0f);

    /// Set face culling mode and front-face orientation.
    SceneVkPipelineBuilder& setCullMode(VkCullModeFlags cullMode,
                                       VkFrontFace frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE);

    /// Configure single-sample multisampling (disabled MSAA).
    SceneVkPipelineBuilder& setMultisamplingNone();

    /// Configure multisampling with specified sample count.
    SceneVkPipelineBuilder& setMultisampling(VkSampleCountFlagBits samples);

    /// Configure color blending to disabled (overwrite).
    SceneVkPipelineBuilder& disableBlending(uint32_t colorAttachmentCount = 1);

    /// Configure standard pre-multiplied / alpha blending.
    SceneVkPipelineBuilder& enableAlphaBlending(uint32_t colorAttachmentCount = 1);

    /// Configure additive blending.
    SceneVkPipelineBuilder& enableAdditiveBlending(uint32_t colorAttachmentCount = 1);

    /// Set an explicit blend state for a specific color attachment.
    SceneVkPipelineBuilder& setColorBlendAttachment(uint32_t index,
                                                    const VkPipelineColorBlendAttachmentState& blendState);

    /// Enable depth testing and depth write with specified compare operator (default VK_COMPARE_OP_GREATER_OR_EQUAL for reversed-Z).
    SceneVkPipelineBuilder& enableDepthTest(bool depthWrite = true,
                                           VkCompareOp compareOp = VK_COMPARE_OP_GREATER_OR_EQUAL);

    /// Disable depth testing and write.
    SceneVkPipelineBuilder& disableDepthTest();

    /// Configure dynamic rendering attachment formats (Vulkan 1.3 / VK_KHR_dynamic_rendering).
    SceneVkPipelineBuilder& setDynamicRendering(const std::vector<VkFormat>& colorFormats,
                                               VkFormat depthFormat = VK_FORMAT_UNDEFINED,
                                               VkFormat stencilFormat = VK_FORMAT_UNDEFINED);

    /// Specify dynamic states (defaults to VIEWPORT and SCISSOR).
    SceneVkPipelineBuilder& setDynamicStates(const std::vector<VkDynamicState>& states);

    /// Build and return the graphics pipeline.
    VkPipeline build(VkDevice device, VkPipelineLayout layout, VkPipelineCache cache = VK_NULL_HANDLE);

private:
    std::vector<VkPipelineShaderStageCreateInfo> shaderStages_;
    std::vector<VkVertexInputBindingDescription> vertexBindings_;
    std::vector<VkVertexInputAttributeDescription> vertexAttributes_;
    VkPipelineInputAssemblyStateCreateInfo inputAssembly_{};
    VkPipelineRasterizationStateCreateInfo rasterizer_{};
    VkPipelineMultisampleStateCreateInfo multisampling_{};
    VkPipelineDepthStencilStateCreateInfo depthStencil_{};
    std::vector<VkPipelineColorBlendAttachmentState> colorBlendAttachments_;
    std::vector<VkDynamicState> dynamicStates_;

    // Dynamic rendering state
    std::vector<VkFormat> colorAttachmentFormats_;
    VkFormat depthAttachmentFormat_ = VK_FORMAT_UNDEFINED;
    VkFormat stencilAttachmentFormat_ = VK_FORMAT_UNDEFINED;
};

/// Wrapper around a VkPipelineCache for caching graphics pipelines.
class SceneVkPipelineCache {
public:
    SceneVkPipelineCache() = default;
    ~SceneVkPipelineCache();

    bool init(VkDevice device, const void* initialData = nullptr, size_t initialSize = 0);
    void destroy();

    VkPipelineCache handle() const { return cache_; }

private:
    VkDevice device_ = VK_NULL_HANDLE;
    VkPipelineCache cache_ = VK_NULL_HANDLE;
};

} // namespace bro::scene::vk
