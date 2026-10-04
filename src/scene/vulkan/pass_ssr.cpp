#include "scene/vulkan/pass_ssr.h"
#include "scene/vulkan/scene_vk_shader_compiler.h"
#include "util/log.h"
#include <cstring>

namespace bro::scene::vk {

PassSSR::~PassSSR() = default;

bool PassSSR::init(SceneVkDevice& device, SceneVkAllocator& allocator) {
    if (!allocator.createUniformBuffer(sizeof(SSRUBOData), ssrUbo_)) {
        LOG_ERROR("PassSSR: Failed creating uniform buffer");
        return false;
    }

    VkSamplerCreateInfo sampInfo{};
    sampInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    sampInfo.magFilter = VK_FILTER_LINEAR;
    sampInfo.minFilter = VK_FILTER_LINEAR;
    sampInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sampInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sampInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    if (vkCreateSampler(device.device(), &sampInfo, nullptr, &sampler_) != VK_SUCCESS) {
        LOG_ERROR("PassSSR: Failed creating sampler");
        return false;
    }

    return createPipeline(device.device());
}

void PassSSR::cleanup(SceneVkDevice& device, SceneVkAllocator& allocator) {
    allocator.destroyBuffer(ssrUbo_);

    VkDevice dev = device.device();
    if (sampler_ != VK_NULL_HANDLE) {
        vkDestroySampler(dev, sampler_, nullptr);
        sampler_ = VK_NULL_HANDLE;
    }
    if (pipeline_ != VK_NULL_HANDLE) {
        vkDestroyPipeline(dev, pipeline_, nullptr);
        pipeline_ = VK_NULL_HANDLE;
    }
    if (pipelineLayout_ != VK_NULL_HANDLE) {
        vkDestroyPipelineLayout(dev, pipelineLayout_, nullptr);
        pipelineLayout_ = VK_NULL_HANDLE;
    }
    if (descLayout_ != VK_NULL_HANDLE) {
        vkDestroyDescriptorSetLayout(dev, descLayout_, nullptr);
        descLayout_ = VK_NULL_HANDLE;
    }
}

bool PassSSR::createPipeline(VkDevice device) {
    SceneVkDescriptorLayoutBuilder descBuilder;
    descBuilder.addBinding(0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT);
    descBuilder.addBinding(1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT);
    descBuilder.addBinding(2, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT);
    descLayout_ = descBuilder.build(device);
    if (!descLayout_) return false;

    VkPipelineLayoutCreateInfo plInfo{};
    plInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    plInfo.setLayoutCount = 1;
    plInfo.pSetLayouts = &descLayout_;
    if (vkCreatePipelineLayout(device, &plInfo, nullptr, &pipelineLayout_) != VK_SUCCESS) return false;

    VkShaderModule vsMod = SceneVkShaderCompiler::createBuiltinModule(device, BuiltinSceneShader::PostFxVert);
    VkShaderModule fsMod = SceneVkShaderCompiler::createBuiltinModule(device, BuiltinSceneShader::SsrFrag);
    if (!vsMod || !fsMod) {
        if (vsMod) SceneVkShaderCompiler::destroyModule(device, vsMod);
        if (fsMod) SceneVkShaderCompiler::destroyModule(device, fsMod);
        return false;
    }

    SceneVkPipelineBuilder builder;
    builder.setShaderStages(vsMod, fsMod)
           .setCullMode(VK_CULL_MODE_NONE)
           .disableBlending(1)
           .disableDepthTest()
           .setDynamicRendering({VK_FORMAT_R16G16B16A16_SFLOAT}, VK_FORMAT_UNDEFINED);

    pipeline_ = builder.build(device, pipelineLayout_);

    SceneVkShaderCompiler::destroyModule(device, vsMod);
    SceneVkShaderCompiler::destroyModule(device, fsMod);

    return pipeline_ != VK_NULL_HANDLE;
}

void PassSSR::render(VkCommandBuffer cmd, SceneVkDevice& device, SceneVkAllocator& allocator,
                     SceneVkDescriptorPool& descPool,
                     const SceneVkImage& colorSnapshot,
                     const SceneVkImage& depthImage,
                     VkImageView outputTargetView,
                     uint32_t width, uint32_t height,
                     const float* projMatrix,
                     const float* invProjMatrix,
                     const SSRParams& params) {
    if (pipeline_ == VK_NULL_HANDLE || params.intensity <= 0.0f) return;

    SSRUBOData uboData{};
    std::memcpy(uboData.proj, projMatrix, sizeof(uboData.proj));
    std::memcpy(uboData.invProj, invProjMatrix, sizeof(uboData.invProj));
    uboData.params1[0] = params.isPerspective ? 1.0f : 0.0f;
    uboData.params1[1] = params.maxDistance;
    uboData.params1[2] = static_cast<float>(params.steps);
    uboData.params1[3] = params.thickness;
    uboData.params2[0] = params.intensity;
    uboData.params2[1] = params.edgeFade;
    allocator.updateUniformBuffer(ssrUbo_, &uboData, sizeof(uboData));

    VkDescriptorSet dSet = descPool.allocate(descLayout_);
    SceneVkDescriptorWriter writer;
    writer.writeImage(0, colorSnapshot.view, sampler_);
    writer.writeImage(1, depthImage.view, sampler_);
    writer.writeBuffer(2, ssrUbo_.buffer, sizeof(SSRUBOData));
    writer.updateSet(device.device(), dSet);

    VkRenderingAttachmentInfo attInfo{};
    attInfo.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
    attInfo.imageView = outputTargetView;
    attInfo.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    attInfo.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    attInfo.storeOp = VK_ATTACHMENT_STORE_OP_STORE;

    VkRenderingInfo rInfo{};
    rInfo.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
    rInfo.renderArea = {{0, 0}, {width, height}};
    rInfo.layerCount = 1;
    rInfo.colorAttachmentCount = 1;
    rInfo.pColorAttachments = &attInfo;

    vkCmdBeginRendering(cmd, &rInfo);

    VkViewport vp{0.0f, 0.0f, static_cast<float>(width), static_cast<float>(height), 0.0f, 1.0f};
    VkRect2D scissor{{0, 0}, {width, height}};
    vkCmdSetViewport(cmd, 0, 1, &vp);
    vkCmdSetScissor(cmd, 0, 1, &scissor);

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout_, 0, 1, &dSet, 0, nullptr);
    vkCmdDraw(cmd, 3, 1, 0, 0);

    vkCmdEndRendering(cmd);
}

} // namespace bro::scene::vk
