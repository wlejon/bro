#pragma once

#include "scene/vulkan/frame/scene_vk_depth.h"
#include "scene/vulkan/frame/scene_vk_target_format.h"

#include <vulkan/vulkan.h>
#include <cstdint>
#include <future>
#include <string>
#include <utility>
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
/// Colour attachments the blend state does not
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

    /// A boolean specialization constant `id` for every stage (a stage that
    /// does not declare it ignores it).
    SceneVkPipelineBuilder& setSpecialization(uint32_t id, bool value);

    /// Build and return the graphics pipeline, through the device's persisted
    /// pipeline cache (render::VulkanPipelineCache).
    VkPipeline build(VkDevice device, VkPipelineLayout layout);

    /// The stages as added, and one stage's module replaced (shader set B's
    /// twin of a pipeline, scene_shader_sets.h).
    const std::vector<VkPipelineShaderStageCreateInfo>& stages() const { return shaderStages_; }
    void setStageModule(size_t index, VkShaderModule module) {
        if (index < shaderStages_.size()) shaderStages_[index].module = module;
    }

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
    std::vector<std::pair<uint32_t, VkBool32>> specializations_;

    // Dynamic rendering state
    std::vector<VkFormat> colorAttachmentFormats_;
    VkFormat depthAttachmentFormat_ = VK_FORMAT_UNDEFINED;
    VkFormat stencilAttachmentFormat_ = VK_FORMAT_UNDEFINED;
};

enum class BuiltinSceneShader;

/// A compute pipeline of the built-in shader `shader` over `layout`, through
/// the device's persisted pipeline cache, its statistics (when on) logged
/// under `name`. Null on failure.
VkPipeline createComputePipeline(VkDevice device, BuiltinSceneShader shader, VkPipelineLayout layout,
                                 const char* name);
/// The same of a shader module the caller keeps.
VkPipeline createComputePipeline(VkDevice device, VkShaderModule module, VkPipelineLayout layout, const char* name);

/// A compute pipeline built on a thread of its own from when it is started
/// (pipeline_async::inFlight counts it), so no frame waits on the driver's
/// compile: get() is null until it is in where frames skip what is not
/// ready (shader_warmup::async()), and waits for it where they do not.
class AsyncComputePipeline {
public:
    /// `module` (start's second form) and `layout` outlive the build.
    void start(VkDevice device, BuiltinSceneShader shader, VkPipelineLayout layout, const char* name);
    void start(VkDevice device, VkShaderModule module, VkPipelineLayout layout, const char* name);
    VkPipeline get();
    /// Started and not yet failed (a null pipeline once built).
    bool usable() const { return started_ && !failed_; }
    bool started() const { return started_; }
    /// Waits for one still being built.
    void destroy(VkDevice device);

private:
    VkPipeline pipeline_ = VK_NULL_HANDLE;
    std::shared_future<VkPipeline> future_;
    bool started_ = false, failed_ = false;
};

} // namespace bro::scene::vk
