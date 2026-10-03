#include "scene/vulkan/pass_shadow.h"
#include "scene/vulkan/scene_vk_shader_compiler.h"
#include "util/log.h"

#include <cstring>

namespace bro::scene::vk {

namespace {

static void mat4Multiply(const float* a, const float* b, float* out) {
    for (int col = 0; col < 4; ++col) {
        for (int row = 0; row < 4; ++row) {
            out[col * 4 + row] =
                a[0 * 4 + row] * b[col * 4 + 0] +
                a[1 * 4 + row] * b[col * 4 + 1] +
                a[2 * 4 + row] * b[col * 4 + 2] +
                a[3 * 4 + row] * b[col * 4 + 3];
        }
    }
}

} // namespace

PassShadow::~PassShadow() {
}

bool PassShadow::init(SceneVkDevice& device) {
    return init(device, Config{});
}

bool PassShadow::init(SceneVkDevice& device, const Config& config) {
    config_ = config;
    VkDevice dev = device.device();

    SceneVkDescriptorLayoutBuilder boneBuilder;
    boneBuilder.addBinding(0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_VERTEX_BIT);
    bonePaletteLayout_ = boneBuilder.build(dev);
    if (!bonePaletteLayout_) return false;

    VkPushConstantRange pushRange{};
    pushRange.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
    pushRange.offset = 0;
    pushRange.size = sizeof(ShadowPushConstants);

    VkPipelineLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layoutInfo.setLayoutCount = 1;
    layoutInfo.pSetLayouts = &bonePaletteLayout_;
    layoutInfo.pushConstantRangeCount = 1;
    layoutInfo.pPushConstantRanges = &pushRange;

    if (vkCreatePipelineLayout(dev, &layoutInfo, nullptr, &pipelineLayout_) != VK_SUCCESS) {
        LOG_ERROR("PassShadow: Failed to create pipeline layout");
        return false;
    }

    if (!createPipelines(dev, config)) {
        LOG_ERROR("PassShadow: Failed to create pipelines");
        return false;
    }

    return true;
}

void PassShadow::cleanup(SceneVkDevice& device) {
    VkDevice dev = device.device();

    if (pipelineStatic_ != VK_NULL_HANDLE) {
        vkDestroyPipeline(dev, pipelineStatic_, nullptr);
        pipelineStatic_ = VK_NULL_HANDLE;
    }
    if (pipelineInstanced_ != VK_NULL_HANDLE) {
        vkDestroyPipeline(dev, pipelineInstanced_, nullptr);
        pipelineInstanced_ = VK_NULL_HANDLE;
    }
    if (pipelineSkinned_ != VK_NULL_HANDLE) {
        vkDestroyPipeline(dev, pipelineSkinned_, nullptr);
        pipelineSkinned_ = VK_NULL_HANDLE;
    }
    if (pipelineLayout_ != VK_NULL_HANDLE) {
        vkDestroyPipelineLayout(dev, pipelineLayout_, nullptr);
        pipelineLayout_ = VK_NULL_HANDLE;
    }
    if (bonePaletteLayout_ != VK_NULL_HANDLE) {
        vkDestroyDescriptorSetLayout(dev, bonePaletteLayout_, nullptr);
        bonePaletteLayout_ = VK_NULL_HANDLE;
    }
}

bool PassShadow::createPipelines(VkDevice device, const Config& config) {
    VkShaderModule vsStatic = SceneVkShaderCompiler::createBuiltinModule(device, BuiltinSceneShader::ShadowVert);
    VkShaderModule vsInst   = SceneVkShaderCompiler::createBuiltinModule(device, BuiltinSceneShader::ShadowInstancedVert);
    VkShaderModule vsSkin   = SceneVkShaderCompiler::createBuiltinModule(device, BuiltinSceneShader::ShadowSkinnedVert);
    VkShaderModule fs       = SceneVkShaderCompiler::createBuiltinModule(device, BuiltinSceneShader::ShadowFrag);

    if (!vsStatic || !vsInst || !vsSkin || !fs) {
        LOG_ERROR("PassShadow: Failed to create shader modules");
        return false;
    }

    // Static shadow vertex layout (only pos needed, but matches static mesh stride 64)
    VkVertexInputBindingDescription staticBinding{};
    staticBinding.binding = 0;
    staticBinding.stride = 64;
    staticBinding.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

    VkVertexInputAttributeDescription posAttr{};
    posAttr.location = 0;
    posAttr.binding = 0;
    posAttr.format = VK_FORMAT_R32G32B32_SFLOAT;
    posAttr.offset = 0;

    // 1. Static shadow pipeline
    SceneVkPipelineBuilder bStatic;
    bStatic.addShaderStage(VK_SHADER_STAGE_VERTEX_BIT, vsStatic)
           .addShaderStage(VK_SHADER_STAGE_FRAGMENT_BIT, fs)
           .setVertexInput({staticBinding}, {posAttr})
           .setInputTopology(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST)
           .setPolygonMode(VK_POLYGON_MODE_FILL)
           .setCullMode(config.cullMode, VK_FRONT_FACE_COUNTER_CLOCKWISE)
           .setMultisamplingNone()
           .disableBlending(0)
           .enableDepthTest(true, VK_COMPARE_OP_LESS_OR_EQUAL)
           .setDynamicStates({VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR, VK_DYNAMIC_STATE_DEPTH_BIAS})
           .setDynamicRendering({}, config.depthFormat);

    pipelineStatic_ = bStatic.build(device, pipelineLayout_);

    // 2. Instanced shadow pipeline
    VkVertexInputBindingDescription instBinding{};
    instBinding.binding = 1;
    instBinding.stride = 64;
    instBinding.inputRate = VK_VERTEX_INPUT_RATE_INSTANCE;

    std::vector<VkVertexInputBindingDescription> instBindings = {staticBinding, instBinding};
    std::vector<VkVertexInputAttributeDescription> instAttrs = {
        posAttr,
        {8,  1, VK_FORMAT_R32G32B32A32_SFLOAT, 0},
        {9,  1, VK_FORMAT_R32G32B32A32_SFLOAT, 16},
        {10, 1, VK_FORMAT_R32G32B32A32_SFLOAT, 32},
        {11, 1, VK_FORMAT_R32G32B32A32_SFLOAT, 48}
    };

    SceneVkPipelineBuilder bInst;
    bInst.addShaderStage(VK_SHADER_STAGE_VERTEX_BIT, vsInst)
         .addShaderStage(VK_SHADER_STAGE_FRAGMENT_BIT, fs)
         .setVertexInput(instBindings, instAttrs)
         .setInputTopology(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST)
         .setPolygonMode(VK_POLYGON_MODE_FILL)
         .setCullMode(config.cullMode, VK_FRONT_FACE_COUNTER_CLOCKWISE)
         .setMultisamplingNone()
         .disableBlending(0)
         .enableDepthTest(true, VK_COMPARE_OP_LESS_OR_EQUAL)
         .setDynamicStates({VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR, VK_DYNAMIC_STATE_DEPTH_BIAS})
         .setDynamicRendering({}, config.depthFormat);

    pipelineInstanced_ = bInst.build(device, pipelineLayout_);

    // 3. Skinned shadow pipeline
    VkVertexInputBindingDescription skinBinding{};
    skinBinding.binding = 1;
    skinBinding.stride = 24;
    skinBinding.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

    std::vector<VkVertexInputBindingDescription> skinBindings = {staticBinding, skinBinding};
    std::vector<VkVertexInputAttributeDescription> skinAttrs = {
        posAttr,
        {5, 1, VK_FORMAT_R16G16B16A16_UINT, 0},
        {6, 1, VK_FORMAT_R32G32B32A32_SFLOAT, 8}
    };

    SceneVkPipelineBuilder bSkin;
    bSkin.addShaderStage(VK_SHADER_STAGE_VERTEX_BIT, vsSkin)
         .addShaderStage(VK_SHADER_STAGE_FRAGMENT_BIT, fs)
         .setVertexInput(skinBindings, skinAttrs)
         .setInputTopology(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST)
         .setPolygonMode(VK_POLYGON_MODE_FILL)
         .setCullMode(config.cullMode, VK_FRONT_FACE_COUNTER_CLOCKWISE)
         .setMultisamplingNone()
         .disableBlending(0)
         .enableDepthTest(true, VK_COMPARE_OP_LESS_OR_EQUAL)
         .setDynamicStates({VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR, VK_DYNAMIC_STATE_DEPTH_BIAS})
         .setDynamicRendering({}, config.depthFormat);

    pipelineSkinned_ = bSkin.build(device, pipelineLayout_);

    SceneVkShaderModule::destroy(device, vsStatic);
    SceneVkShaderModule::destroy(device, vsInst);
    SceneVkShaderModule::destroy(device, vsSkin);
    SceneVkShaderModule::destroy(device, fs);

    return (pipelineStatic_ != VK_NULL_HANDLE &&
            pipelineInstanced_ != VK_NULL_HANDLE &&
            pipelineSkinned_ != VK_NULL_HANDLE);
}

void PassShadow::beginCascade(VkCommandBuffer cmd, SceneVkDevice& device,
                             SceneVkShadowCascadeTarget& target,
                             uint32_t cascadeIndex,
                             const float* lightViewProjMatrix) {
    std::memcpy(activeLightVP_, lightViewProjMatrix, sizeof(activeLightVP_));
    resolution_ = target.resolution();

    target.beginCascadeRendering(cmd, device, cascadeIndex, 1.0f);

    VkViewport vp{};
    vp.x = 0.0f;
    vp.y = 0.0f;
    vp.width = static_cast<float>(resolution_);
    vp.height = static_cast<float>(resolution_);
    vp.minDepth = 0.0f;
    vp.maxDepth = 1.0f;
    vkCmdSetViewport(cmd, 0, 1, &vp);

    VkRect2D scissor{};
    scissor.offset = {0, 0};
    scissor.extent = {resolution_, resolution_};
    vkCmdSetScissor(cmd, 0, 1, &scissor);

    vkCmdSetDepthBias(cmd, config_.depthBiasConstant, config_.depthBiasClamp, config_.depthBiasSlope);
}

void PassShadow::drawStatic(VkCommandBuffer cmd, const ShadowCaster& caster) {
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineStatic_);

    ShadowPushConstants push{};
    mat4Multiply(activeLightVP_, caster.modelMatrix, push.lightMVP);
    vkCmdPushConstants(cmd, pipelineLayout_, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(push), &push);

    vkCmdBindVertexBuffers(cmd, 0, 1, &caster.vertexBuffer, &caster.vertexOffset);
    vkCmdBindIndexBuffer(cmd, caster.indexBuffer, caster.indexOffset, caster.indexType);

    vkCmdDrawIndexed(cmd, caster.indexCount, 1, 0, 0, 0);
}

void PassShadow::drawInstanced(VkCommandBuffer cmd, const InstancedShadowCaster& caster) {
    if (caster.instanceCount == 0) return;

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineInstanced_);

    ShadowPushConstants push{};
    mat4Multiply(activeLightVP_, caster.modelMatrix, push.lightMVP);
    vkCmdPushConstants(cmd, pipelineLayout_, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(push), &push);

    VkBuffer buffers[2] = {caster.vertexBuffer, caster.instanceBuffer};
    VkDeviceSize offsets[2] = {caster.vertexOffset, caster.instanceOffset};
    vkCmdBindVertexBuffers(cmd, 0, 2, buffers, offsets);
    vkCmdBindIndexBuffer(cmd, caster.indexBuffer, caster.indexOffset, caster.indexType);

    vkCmdDrawIndexed(cmd, caster.indexCount, caster.instanceCount, 0, 0, 0);
}

void PassShadow::drawSkinned(VkCommandBuffer cmd, const SkinnedShadowCaster& caster) {
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineSkinned_);

    if (caster.bonePaletteSet) {
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout_, 0, 1,
                               &caster.bonePaletteSet, 0, nullptr);
    }

    ShadowPushConstants push{};
    mat4Multiply(activeLightVP_, caster.modelMatrix, push.lightMVP);
    vkCmdPushConstants(cmd, pipelineLayout_, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(push), &push);

    VkBuffer buffers[2] = {caster.vertexBuffer, caster.skinAttribBuffer};
    VkDeviceSize offsets[2] = {caster.vertexOffset, caster.skinAttribOffset};
    vkCmdBindVertexBuffers(cmd, 0, 2, buffers, offsets);
    vkCmdBindIndexBuffer(cmd, caster.indexBuffer, caster.indexOffset, caster.indexType);

    vkCmdDrawIndexed(cmd, caster.indexCount, 1, 0, 0, 0);
}

void PassShadow::endCascade(VkCommandBuffer cmd, SceneVkDevice& device,
                           SceneVkShadowCascadeTarget& target) {
    target.endCascadeRendering(cmd, device);
}

} // namespace bro::scene::vk
