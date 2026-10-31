#include "scene/vulkan/frame/scene_vk_pipeline.h"
#include "render/vulkan_pipeline_cache.h"
#include "scene/vulkan/frame/scene_shader_sets.h"
#include "scene/vulkan/frame/scene_cpu_stalls.h"
#include "util/log.h"

#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <future>
#include <memory>

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
    shader_sets::noteModule(module, code, sizeBytes);
    return module;
}

VkShaderModule SceneVkShaderModule::create(VkDevice device, const std::vector<uint32_t>& spirv) {
    return create(device, spirv.data(), spirv.size() * sizeof(uint32_t));
}

void SceneVkShaderModule::destroy(VkDevice device, VkShaderModule module) {
    if (device != VK_NULL_HANDLE && module != VK_NULL_HANDLE) {
        shader_sets::forgetModule(module);
        vkDestroyShaderModule(device, module, nullptr);
    }
}

SceneVkPipelineBuilder::SceneVkPipelineBuilder() {
    reset();
}

SceneVkPipelineBuilder& SceneVkPipelineBuilder::reset() {
    shaderStages_.clear();
    stageEntryNames_.clear();
    specializations_.clear();
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
    stageEntryNames_.push_back(entryPoint ? entryPoint : "main");
    stageInfo.pName = stageEntryNames_.back().c_str();
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

SceneVkPipelineBuilder& SceneVkPipelineBuilder::setSpecialization(uint32_t id, bool value) {
    specializations_.emplace_back(id, value ? VK_TRUE : VK_FALSE);
    return *this;
}

namespace pipeline_async {
namespace {
std::atomic<int> s_inFlight{0};
std::atomic<int> s_skips{0};
std::atomic<bool> s_async{false};
}
void setAsync(bool on) {
    const char* v = std::getenv("BRO_ASYNC_SHADERS");
    if (v && *v) on = std::strcmp(v, "0") != 0;
    s_async.store(on, std::memory_order_relaxed);
}
void forceAsync(bool on) { s_async.store(on, std::memory_order_relaxed); }
bool async() { return s_async.load(std::memory_order_relaxed); }
Capture*& current() {
    thread_local Capture* c = nullptr;
    return c;
}
int inFlight() { return s_inFlight.load(std::memory_order_relaxed); }
void noteSkip() { s_skips.fetch_add(1, std::memory_order_relaxed); }
int takeSkips() { return s_skips.exchange(0, std::memory_order_relaxed); }
}  // namespace pipeline_async

VkPipeline SceneVkPipelineBuilder::build(VkDevice device, VkPipelineLayout layout) {
    if (pipeline_async::Capture* c = pipeline_async::current(); c && !c->used && pipeline_async::async()) {
        // Built on a thread of its own from a copy of this builder (the
        // create-info points into it); that thread has no capture, so the
        // copy builds there as below.
        c->used = true;
        auto copy = std::make_shared<SceneVkPipelineBuilder>(*this);
        pipeline_async::s_inFlight.fetch_add(1, std::memory_order_relaxed);
        c->future = std::async(std::launch::async, [copy, device, layout] {
                        VkPipeline p = copy->build(device, layout);
                        pipeline_async::s_inFlight.fetch_sub(1, std::memory_order_relaxed);
                        return p;
                    }).share();
        return VK_NULL_HANDLE;
    }
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

    // The builder's specialization constants, on every stage.
    std::vector<VkBool32> specData;
    std::vector<VkSpecializationMapEntry> specEntries;
    for (const auto& [id, value] : specializations_) {
        specEntries.push_back({id, static_cast<uint32_t>(specData.size() * sizeof(VkBool32)), sizeof(VkBool32)});
        specData.push_back(value);
    }
    VkSpecializationInfo spec{};
    spec.mapEntryCount = static_cast<uint32_t>(specEntries.size());
    spec.pMapEntries = specEntries.data();
    spec.dataSize = specData.size() * sizeof(VkBool32);
    spec.pData = specData.data();
    std::vector<VkPipelineShaderStageCreateInfo> stages = shaderStages_;
    for (size_t i = 0; i < stages.size() && i < stageEntryNames_.size(); ++i)
        stages[i].pName = stageEntryNames_[i].c_str();
    if (!specEntries.empty())
        for (auto& st : stages) st.pSpecializationInfo = &spec;

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
    pipelineInfo.flags |= render::pipelineStatsFlags();

    VkPipeline pipeline = VK_NULL_HANDLE;
    const double t0 = stalls::nowMs();
    VkResult res = vkCreateGraphicsPipelines(device, render::VulkanPipelineCache::forDevice(device), 1,
                                             &pipelineInfo, nullptr, &pipeline);
    stalls::notePipeline(stalls::nowMs() - t0);
    if (res != VK_SUCCESS) {
        LOG_ERROR("SceneVkPipelineBuilder: Failed to create graphics pipeline: %d", res);
        return VK_NULL_HANDLE;
    }
    render::logPipelineStats(device, pipeline, label_.empty() ? std::string("graphics") : label_);
    shader_sets::noteGraphicsPipeline(device, layout, *this, pipeline);

    return pipeline;
}

VkPipeline createComputePipeline(VkDevice device, VkShaderModule module, VkPipelineLayout layout, const char* name,
                                 const char* entry) {
    if (!module || !layout) return VK_NULL_HANDLE;
    VkComputePipelineCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    ci.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    ci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    ci.stage.module = module;
    ci.stage.pName = entry;
    ci.layout = layout;
    ci.flags = render::pipelineStatsFlags();
    VkPipeline pipeline = VK_NULL_HANDLE;
    const double t0 = stalls::nowMs();
    const VkResult res =
        vkCreateComputePipelines(device, render::VulkanPipelineCache::forDevice(device), 1, &ci, nullptr, &pipeline);
    stalls::notePipeline(stalls::nowMs() - t0);
    if (res != VK_SUCCESS) {
        LOG_ERROR("createComputePipeline: Failed creating %s", name);
        pipeline = VK_NULL_HANDLE;
    } else {
        render::logPipelineStats(device, pipeline, name);
        shader_sets::noteComputePipeline(device, layout, module, name, entry, pipeline);
    }
    return pipeline;
}

namespace {
template <typename Build>
std::shared_future<VkPipeline> buildOnThread(Build build) {
    pipeline_async::s_inFlight.fetch_add(1, std::memory_order_relaxed);
    return std::async(std::launch::async, [build] {
               VkPipeline p = build();
               pipeline_async::s_inFlight.fetch_sub(1, std::memory_order_relaxed);
               return p;
           }).share();
}
}  // namespace

void AsyncComputePipeline::start(VkDevice device, VkShaderModule module, VkPipelineLayout layout, const char* name,
                                 const char* entry) {
    std::string e(entry ? entry : "main");
    future_ = buildOnThread([=] { return createComputePipeline(device, module, layout, name, e.c_str()); });
    started_ = true;
    failed_ = false;
}

VkPipeline AsyncComputePipeline::get() {
    if (pipeline_ || !future_.valid()) return pipeline_;
    if (future_.wait_for(std::chrono::seconds(0)) != std::future_status::ready) {
        if (pipeline_async::async()) {
            pipeline_async::noteSkip();
            return VK_NULL_HANDLE;
        }
        // (Headless: the frame holds everything it draws.)
        const double t0 = stalls::nowMs();
        future_.wait();
        stalls::notePipeline(stalls::nowMs() - t0);
    }
    pipeline_ = future_.get();
    future_ = {};
    failed_ = pipeline_ == VK_NULL_HANDLE;
    return pipeline_;
}

void AsyncComputePipeline::destroy(VkDevice device) {
    if (future_.valid()) pipeline_ = future_.get();
    future_ = {};
    if (pipeline_) destroyScenePipeline(device, pipeline_, nullptr);
    pipeline_ = VK_NULL_HANDLE;
    started_ = failed_ = false;
}

} // namespace bro::scene::vk
