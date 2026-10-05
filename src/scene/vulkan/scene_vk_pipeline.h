#pragma once

#include "scene/vulkan/scene_vk_depth.h"
#include "scene/vulkan/scene_vk_target_format.h"

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
///
/// Every stage gets the depth policy as specialization constant
/// depth::kReversedZConstantId. Colour attachments the blend state does not
/// mention (a target wider than what the shader writes) get a zero write
/// mask, so a pass can draw into a scope that carries extra attachments.
class SceneVkPipelineBuilder {
public:
    SceneVkPipelineBuilder();
    ~SceneVkPipelineBuilder() = default;

    /// Reset all state to default values.
    SceneVkPipelineBuilder& reset();

    /// Attach vertex and fragment shader stages.
    SceneVkPipelineBuilder& setShaderStages(VkShaderModule vs, VkShaderModule fs) {
        addShaderStage(VK_SHADER_STAGE_VERTEX_BIT, vs);
        addShaderStage(VK_SHADER_STAGE_FRAGMENT_BIT, fs);
        return *this;
    }

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

    /// Enable depth bias; its factors are set with vkCmdSetDepthBias (list
    /// VK_DYNAMIC_STATE_DEPTH_BIAS among the dynamic states).
    SceneVkPipelineBuilder& setDepthBias(bool enable);

    /// Attachment formats and sample count of the target the pipeline draws into.
    SceneVkPipelineBuilder& setTarget(const TargetFormat& target);

    /// Configure color blending to disabled (overwrite).
    SceneVkPipelineBuilder& disableBlending(uint32_t colorAttachmentCount = 1);

    /// Configure standard pre-multiplied / alpha blending.
    SceneVkPipelineBuilder& enableAlphaBlending(uint32_t colorAttachmentCount = 1);

    /// Set an explicit blend state for a specific color attachment.
    SceneVkPipelineBuilder& setColorBlendAttachment(uint32_t index,
                                                    const VkPipelineColorBlendAttachmentState& blendState);

    /// Enable depth testing; the compare op defaults to the camera depth
    /// policy's "nearer wins" (depth::compareCloser).
    SceneVkPipelineBuilder& enableDepthTest(bool depthWrite = true,
                                           VkCompareOp compareOp = depth::compareCloser());

    /// Disable depth testing and write.
    SceneVkPipelineBuilder& disableDepthTest();

    /// Specify dynamic states (defaults to VIEWPORT and SCISSOR).
    SceneVkPipelineBuilder& setDynamicStates(const std::vector<VkDynamicState>& states);

    /// Build and return the graphics pipeline, through the device's persisted
    /// pipeline cache (render::VulkanPipelineCache).
    VkPipeline build(VkDevice device, VkPipelineLayout layout);

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

} // namespace bro::scene::vk
